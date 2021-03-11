/*
 * callback.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */
#pragma once
#include <inttypes.h>
#include <errno.h>
#include <type_traits>
#include "utility.h"
#include "PMF.h"

struct Delegate_Base {};

// This is a functor class that represents either a plain function of
// `R (*)(void*, Ts...)`, or a member function of class U `R (U::*)(Ts...)`.
// std::function<void(Ts...)> is not used, as it's less efficient.
template <typename R, typename... Ts>
struct Delegate : public Delegate_Base {
    typedef R (*Func)(void *, Ts...);
    void *_obj = nullptr;
    Func _func = nullptr;

    Delegate() = default;

    template <typename U> // U's Member Function
    using UMFunc = R (U::*)(Ts...);

    template <typename U> // U's const Member Function
    using UCMFunc = R (U::*)(Ts...) const;

    template <typename U> // Function with U* as the 1st argument
    using UFunc = R (*)(U *, Ts...);

    Delegate(void *obj, Func func) {
        bind(obj, func);
    }

    Delegate(Func func, void *obj) {
        bind(obj, func);
    }

    template <typename U>
    Delegate(U *obj, UFunc<U> func) {
        bind(obj, func);
    }

    template <typename U>
    Delegate(U *obj, UMFunc<U> func) {
        bind(obj, func);
    }

    template <typename U>
    Delegate(U *obj, UCMFunc<U> func) {
        bind(obj, func);
    }

    template <typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    Delegate(T *obj, UMFunc<U> func) {
        bind<U>(obj, func);
    }

    template <typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    Delegate(T *obj, UCMFunc<U> func) {
        bind<U>(obj, func);
    }

#define ENABLE_IF_NOT_CB(U) ENABLE_IF_NOT_BASE_OF(Delegate_Base, U)

    template <typename U, ENABLE_IF_NOT_CB(U)>
    /*explicit*/ Delegate(U &obj) {
        bind(&obj, &U::operator());
    }

    void bind(void *obj, Func func) {
        _obj = obj;
        _func = func;
    }

    void bind(Func func, void *obj) {
        _obj = obj;
        _func = func;
    }

    template <typename U>
    void bind(U *obj, UFunc<U> func) {
        bind(obj, (Func &)func);
    }

    template <typename U>
    void bind(U *obj, UMFunc<U> func) {
        bind(obj, (UCMFunc<U> &)func);
    }

    template <typename U>
    void bind(U *obj, UCMFunc<U> func) {
        auto pmf = ::get_member_function_address(obj, func);
        _func = (Func &)pmf.f;
        _obj = (void *)pmf.obj;
    }

    template <typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    void bind(T *obj, UMFunc<U> func) {
        bind<U>(obj, func);
    }

    template <typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    void bind(T *obj, UCMFunc<U> func) {
        bind<U>(obj, func);
    }

    template <typename U, ENABLE_IF_NOT_CB(U)>
    void bind(U &obj) {
        bind<U>(&obj, &U::operator());
    }

    void bind(const Delegate &rhs) {
        _func = rhs._func;
        _obj = rhs._obj;
    }

    template <typename U, ENABLE_IF_NOT_CB(U)>
    void bind(U &&obj) = delete; // not allow to bind to a rvalue (temp) object

    void clear() {
        _func = nullptr;
    }

    operator bool() const {
        return _func;
    }

    R operator()(const Ts &...args) const {
        return fire(args...);
    }

    R fire(const Ts &...args) const {
        return _func ? _func(_obj, args...) : R();
    }

    R fire_once(const Ts &...args) {
        auto ret = fire(args...);
        clear();
        return ret;
    }
};

template <typename... Ts>
using Callback = Delegate<int, Ts...>;

inline int __Examples_of_Callback(void *, int, double, long) {
    static char ptr[] = "example";
    typedef Callback<int, double, long> Callback;
    Callback cb1(ptr, &__Examples_of_Callback);
    cb1.bind(ptr, &__Examples_of_Callback);
    cb1(65, 43.21, 0);

    class Example {
    public:
        int example(int, double, long) {
            return 0;
        }
        virtual int example2(int, double, long) {
            return -1;
        }
    };

    // lambda MUST be assigned to a variable before binding,
    // and MUST remain valid during the life-cycle of bond Callback!
    auto lambda = [&](int, double, long) { return 0; };

    Example e;
    Callback cb2(&e, &Example::example);
    cb2.fire_once(65, 43.21, 0); // equivalent to e.example(65, 43.21, 0);
    cb2.bind(&e, &Example::example2);
    cb2.bind(lambda);
    lambda(1, 2, 3);

    return 0;
}
