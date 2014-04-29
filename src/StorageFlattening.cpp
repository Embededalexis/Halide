#include <sstream>

#include "StorageFlattening.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"

namespace Halide {
namespace Internal {

using std::ostringstream;
using std::string;
using std::vector;
using std::map;

class FlattenDimensions : public IRMutator {
public:
    FlattenDimensions(const map<string, Function> &e) : env(e) {}
    Scope<int> scope;
    Scope<int> need_buffer_t;
private:
    const map<string, Function> &env;

    Expr flatten_args(const string &name, const vector<Expr> &args) {
        Expr idx = 0;
        vector<Expr> mins(args.size()), strides(args.size());

        for (size_t i = 0; i < args.size(); i++) {
            string dim = int_to_string(i);
            string stride_name = name + ".stride." + dim;
            string min_name = name + ".min." + dim;
            string stride_name_constrained = stride_name + ".constrained";
            string min_name_constrained = min_name + ".constrained";
            if (scope.contains(stride_name_constrained)) {
                stride_name = stride_name_constrained;
            }
            if (scope.contains(min_name_constrained)) {
                min_name = min_name_constrained;
            }
            strides[i] = Variable::make(Int(32), stride_name);
            mins[i] = Variable::make(Int(32), min_name);
        }

        if (env.find(name) != env.end()) {
            // f(x, y) -> f[(x-xmin)*xstride + (y-ymin)*ystride] This
            // strategy makes sense when we expect x to cancel with
            // something in xmin.  We use this for internal allocations
            for (size_t i = 0; i < args.size(); i++) {
                idx += (args[i] - mins[i]) * strides[i];
            }
        } else {
            // f(x, y) -> f[x*stride + y*ystride - (xstride*xmin +
            // ystride*ymin)]. The idea here is that the last term
            // will be pulled outside the inner loop. We use this for
            // external buffers, where the mins and strides are likely
            // to be symbolic
            Expr base = 0;
            for (size_t i = 0; i < args.size(); i++) {
                idx += args[i] * strides[i];
                base += mins[i] * strides[i];
            }
            idx -= base;
        }

        return idx;
    }

    using IRMutator::visit;

    void visit(const Realize *realize) {
        Stmt body = mutate(realize->body);

        // Check if we need to create a buffer_t for this realization
        vector<bool> make_buffer_t(realize->types.size());
        while (need_buffer_t.contains(realize->name)) {
            int idx = need_buffer_t.get(realize->name);
            internal_assert(idx < (int)make_buffer_t.size());
            make_buffer_t[idx] = true;
            need_buffer_t.pop(realize->name);
        }

        // Compute the size
        std::vector<Expr> extents;
        for (size_t i = 0; i < realize->bounds.size(); i++) {
          extents.push_back(realize->bounds[i].extent);
          extents[i] = mutate(extents[i]);
        }

        vector<int> storage_permutation;
        {
            map<string, Function>::const_iterator iter = env.find(realize->name);
            internal_assert(iter != env.end()) << "Realize node refers to function not in environment.\n";
            const vector<string> &storage_dims = iter->second.schedule().storage_dims;
            const vector<string> &args = iter->second.args();
            for (size_t i = 0; i < storage_dims.size(); i++) {
                for (size_t j = 0; j < args.size(); j++) {
                    if (args[j] == storage_dims[i]) {
                        storage_permutation.push_back((int)j);
                    }
                }
                internal_assert(storage_permutation.size() == i+1);
            }
        }

        internal_assert(storage_permutation.size() == realize->bounds.size());

        stmt = body;
        for (size_t idx = 0; idx < realize->types.size(); idx++) {
            string buffer_name = realize->name;
            if (realize->types.size() > 1) {
                buffer_name = buffer_name + '.' + int_to_string(idx);
            }

            // Make the names for the mins, extents, and strides
            int dims = realize->bounds.size();
            vector<string> min_name(dims), extent_name(dims), stride_name(dims);
            for (int i = 0; i < dims; i++) {
                string d = int_to_string(i);
                min_name[i] = buffer_name + ".min." + d;
                stride_name[i] = buffer_name + ".stride." + d;
                extent_name[i] = buffer_name + ".extent." + d;
            }
            vector<Expr> min_var(dims), extent_var(dims), stride_var(dims);
            for (int i = 0; i < dims; i++) {
                min_var[i] = Variable::make(Int(32), min_name[i]);
                extent_var[i] = Variable::make(Int(32), extent_name[i]);
                stride_var[i] = Variable::make(Int(32), stride_name[i]);
            }

            // Promote the type to be a multiple of 8 bits
            Type t = realize->types[idx];
            t.bits = t.bytes() * 8;

            // Make the allocation node
            stmt = Allocate::make(buffer_name, t, extents, stmt);

            // Create a buffer_t object if necessary. The corresponding let is
            // placed before the allocation node so that the buffer_t is
            // already on the symbol table when doing the allocation.
            if (make_buffer_t[idx]) {
                vector<Expr> args(dims*3 + 2);
                args[0] = Call::make(Handle(), Call::null_handle, vector<Expr>(), Call::Intrinsic);
                args[1] = realize->types[idx].bytes();
                for (int i = 0; i < dims; i++) {
                    args[3*i+2] = min_var[i];
                    args[3*i+3] = extent_var[i];
                    args[3*i+4] = stride_var[i];
                }
                Expr buf = Call::make(Handle(), Call::create_buffer_t,
                                      args, Call::Intrinsic);
                stmt = LetStmt::make(buffer_name + ".buffer",
                                     buf,
                                     stmt);
            }

            // Compute the strides
            for (int i = (int)realize->bounds.size()-1; i > 0; i--) {
                int prev_j = storage_permutation[i-1];
                int j = storage_permutation[i];
                Expr stride = stride_var[prev_j] * extent_var[prev_j];
                stmt = LetStmt::make(stride_name[j], stride, stmt);
            }
            // Innermost stride is one
            if (dims > 0) {
                int innermost = storage_permutation.empty() ? 0 : storage_permutation[0];
                stmt = LetStmt::make(stride_name[innermost], 1, stmt);
            }

            // Assign the mins and extents stored
            for (size_t i = realize->bounds.size(); i > 0; i--) {
                stmt = LetStmt::make(min_name[i-1], realize->bounds[i-1].min, stmt);
                stmt = LetStmt::make(extent_name[i-1], realize->bounds[i-1].extent, stmt);
            }
        }
    }

    void visit(const Provide *provide) {

        vector<Expr> values(provide->values.size());
        for (size_t i = 0; i < values.size(); i++) {
            values[i] = mutate(provide->values[i]);

            // Promote the type to be a multiple of 8 bits
            Type t = values[i].type();
            t.bits = t.bytes() * 8;
            if (t.bits != values[i].type().bits) {
                values[i] = Cast::make(t, values[i]);
            }
        }

        if (values.size() == 1) {
            Expr idx = mutate(flatten_args(provide->name, provide->args));
            stmt = Store::make(provide->name, values[0], idx);
        } else {

            vector<string> names(provide->values.size());
            Stmt result;

            // Store the values by name
            for (size_t i = 0; i < provide->values.size(); i++) {
                string name = provide->name + "." + int_to_string(i);
                Expr idx = mutate(flatten_args(name, provide->args));
                names[i] = name + ".value";
                Expr var = Variable::make(values[i].type(), names[i]);
                Stmt store = Store::make(name, var, idx);

                if (result.defined()) {
                    result = Block::make(result, store);
                } else {
                    result = store;
                }
            }

            // Add the let statements that define the values
            for (size_t i = provide->values.size(); i > 0; i--) {
                result = LetStmt::make(names[i-1], values[i-1], result);
            }

            stmt = result;
        }
    }

    void visit(const Call *call) {

        if (call->call_type == Call::Extern || call->call_type == Call::Intrinsic) {
            vector<Expr> args(call->args.size());
            bool changed = false;
            for (size_t i = 0; i < args.size(); i++) {
                args[i] = mutate(call->args[i]);
                if (!args[i].same_as(call->args[i])) changed = true;
            }
            if (!changed) {
                expr = call;
            } else {
                expr = Call::make(call->type, call->name, args, call->call_type);
            }
        } else {
            string name = call->name;
            if (call->call_type == Call::Halide &&
                call->func.outputs() > 1) {
                name = name + '.' + int_to_string(call->value_index);
            }

            // Promote the type to be a multiple of 8 bits
            Type t = call->type;
            t.bits = t.bytes() * 8;

            Expr idx = mutate(flatten_args(name, call->args));
            expr = Load::make(t, name, idx, call->image, call->param);

            if (call->type.bits != t.bits) {
                expr = Cast::make(call->type, expr);
            }
        }
    }

    void visit(const LetStmt *let) {
        // Discover constrained versions of things.
        bool constrained_version_exists = ends_with(let->name, ".constrained");
        if (constrained_version_exists) {
            scope.push(let->name, 0);
        }

        IRMutator::visit(let);

        if (constrained_version_exists) {
            scope.pop(let->name);
        }
    }
};

class CreateOpenGLLoads : public IRMutator {
public:
    CreateOpenGLLoads() {
        inside_kernel_loop = false;
    }
    Scope<int> scope;
    Scope<int> need_buffer_t;
    bool inside_kernel_loop;
private:
    using IRMutator::visit;

    static float max_value(const Type &type) {
        if (type == UInt(8)) {
            return 255.0f;
        } else if (type == UInt(16)) {
            return 65535.0f;
        } else {
            internal_error << "Cannot determine max_value of type '" << type << "'\n";
        }
        return 1.0f;
    }

    void visit(const Provide *provide) {
        if (!inside_kernel_loop) {
            IRMutator::visit(provide);
            return;
        }

        internal_assert(provide->values.size() == 1) << "GLSL currently only supports scalar stores.\n";
        user_assert(provide->args.size() == 3) << "GLSL stores requires three coordinates.\n";

        // Record that this buffer is accessed from a GPU kernel
        need_buffer_t.push(provide->name, 0);

        // Create glsl_texture_store(name, x, y, c, value, name.buffer)
        // intrinsic.  Since the intrinsic only stores Float(32) values, the
        // original value type is encoded in first argument.
        vector<Expr> args(6);
        Expr value = mutate(provide->values[0]);
        args[0] = Variable::make(value.type(), provide->name);
        for (size_t i = 0; i < provide->args.size(); i++) {
            args[i + 1] = provide->args[i];
        }
        args[4] = Div::make(Cast::make(Float(32), value),
                            max_value(value.type()));
        args[5] = Variable::make(Handle(), provide->name + ".buffer");
        stmt = Evaluate::make(
            Call::make(Float(32), "glsl_texture_store", args, Call::Intrinsic));
    }

    void visit(const Call *call) {
        if (!inside_kernel_loop || call->call_type == Call::Intrinsic) {
            IRMutator::visit(call);
            return;
        }

        string name = call->name;
        if (call->call_type == Call::Halide && call->func.outputs() > 1) {
            name = name + '.' + int_to_string(call->value_index);
        }

        user_assert(call->args.size() == 3) << "GLSL loads requires three coordinates.\n";

        // Record that this buffer is accessed from a GPU kernel
        need_buffer_t.push(call->name, 0);

        // Create glsl_texture_load(name, x, y, c, name.buffer) intrinsic.
        // Since the intrinsic always returns Float(32), the original type is
        // encoded in first argument.
        vector<Expr> args(5);
        args[0] = Variable::make(call->type, call->name);
        for (size_t i = 0; i < call->args.size(); i++) {
            string d = int_to_string(i);
            string min_name = name + ".min." + d;
            string min_name_constrained = min_name + ".constrained";
            if (scope.contains(min_name_constrained)) {
                min_name = min_name_constrained;
            }
            string extent_name = name + ".extent." + d;
            string extent_name_constrained = extent_name + ".constrained";
            if (scope.contains(extent_name_constrained)) {
                extent_name = extent_name_constrained;
            }

            Expr min = Variable::make(Int(32), min_name);
            Expr extent = Variable::make(Int(32), extent_name);

            // Normalize the two spatial coordinates x,y
            args[i + 1] = (i < 2)
                ? (Cast::make(Float(32), call->args[i] - min) + 0.5f) / extent
                : call->args[i] - min;
        }
        args[4] = Variable::make(Handle(), call->name + ".buffer");

        Expr load = Call::make(Float(32), "glsl_texture_load",
                               args, Call::Intrinsic,
                               Function(), 0, call->image, call->param);
        expr = Cast::make(call->type,
                          Mul::make(load, max_value(call->type)));
    }

    void visit(const LetStmt *let) {
        // Discover constrained versions of things.
        bool constrained_version_exists = ends_with(let->name, ".constrained");
        if (constrained_version_exists) {
            scope.push(let->name, 0);
        }

        IRMutator::visit(let);

        if (constrained_version_exists) {
            scope.pop(let->name);
        }
    }

    void visit(const For *loop) {
        bool old_kernel_loop = inside_kernel_loop;
        if (loop->for_type == For::Parallel &&
            (ends_with(loop->name, ".blockidx") || ends_with(loop->name, ".blockidy"))) {
            inside_kernel_loop = true;
        }
        IRMutator::visit(loop);
        inside_kernel_loop = old_kernel_loop;
    }
};


Stmt storage_flattening(Stmt s, const map<string, Function> &env, const Target &target) {
    if (target.features & Target::OpenGL) {
        CreateOpenGLLoads opengl_loads;
        s = opengl_loads.mutate(s);
        FlattenDimensions flatten(env);
        flatten.need_buffer_t = opengl_loads.need_buffer_t;
        return flatten.mutate(s);
    }
    return FlattenDimensions(env).mutate(s);
}

}
}
