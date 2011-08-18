/* -*- mode: c; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: t -*-
 * vim: ts=4 sw=4 noet ai cindent syntax=cpp
 *
 * luamm:  C++ binding for lua
 *
 * Copyright (C) 2010 Pavel Labath et al.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef LUAMM_HH
#define LUAMM_HH

#include <assert.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <lua.hpp>

namespace lua {
	class state;

	typedef lua_Integer integer;
	typedef lua_Number number;
	typedef std::function<int(state *)> cpp_function;

	enum {
		ENVIRONINDEX = LUA_ENVIRONINDEX,
		GLOBALSINDEX = LUA_GLOBALSINDEX,
		REGISTRYINDEX = LUA_REGISTRYINDEX
	};

	enum {
		GCSTOP		 = LUA_GCSTOP,
		GCRESTART	 = LUA_GCRESTART,
		GCCOLLECT	 = LUA_GCCOLLECT,
		GCCOUNT		 = LUA_GCCOUNT,
		GCCOUNTB	 = LUA_GCCOUNTB,
		GCSTEP		 = LUA_GCSTEP,
		GCSETPAUSE	 = LUA_GCSETPAUSE,
		GCSETSTEPMUL = LUA_GCSETSTEPMUL
	};

	enum {
		MULTRET = LUA_MULTRET
	};

	enum Type {
		TBOOLEAN	   = LUA_TBOOLEAN,
		TFUNCTION	   = LUA_TFUNCTION,
		TLIGHTUSERDATA = LUA_TLIGHTUSERDATA,
		TNIL		   = LUA_TNIL,
		TNONE		   = LUA_TNONE,
		TNUMBER		   = LUA_TNUMBER,
		TSTRING		   = LUA_TSTRING,
		TTABLE		   = LUA_TTABLE,
		TTHREAD		   = LUA_TTHREAD,
		TUSERDATA	   = LUA_TUSERDATA
	};

	// we reserve one upvalue for the function pointer
	inline int upvalueindex(int n)
	{ return lua_upvalueindex(n+1); }

	/*
	 * Lua error()s are wrapped in this class when rethrown into C++ code. what() returns the
	 * error message. push_lua_error() pushes the error onto lua stack. The error can only be
	 * pushed into the same state it was generated in.
	 */
	class exception: public std::runtime_error {
		/*
		 * We only allow moving, to avoid complications with multiple references. It shouldn't be
		 * difficult to modify this to work with copying, if that proves unavoidable.
		 */
		state *L;
		std::shared_ptr<const bool> L_valid;
		int key;

		static std::string get_error_msg(state *L);

		exception(const exception &) = delete;
		const exception& operator=(const exception &) = delete;

	public:
		exception(exception &&other)
			: std::runtime_error(std::move(other)), L(other.L),
			  L_valid(std::move(other.L_valid)), key(other.key)
		{ other.L = NULL; }

		explicit exception(state *l);
		virtual ~exception() throw();

		void push_lua_error(state *l);
	};

	class not_string_error: public std::runtime_error {
	public:
		not_string_error()
			: std::runtime_error("Cannot convert value to a string")
		{}
	};

	// the name says it all
	class syntax_error: public lua::exception {
		syntax_error(const syntax_error &) = delete;
		const syntax_error& operator=(const syntax_error &) = delete;

	public:
		syntax_error(state *L)
			: lua::exception(L)
		{}

		syntax_error(syntax_error &&other)
			: lua::exception(std::move(other))
		{}
	};

	// loadfile() encountered an error while opening/reading the file
	class file_error: public lua::exception {
		file_error(const file_error &) = delete;
		const file_error& operator=(const file_error &) = delete;

	public:
		file_error(state *L)
			: lua::exception(L)
		{}

		file_error(file_error &&other)
			: lua::exception(std::move(other))
		{}
	};

	// double fault, lua encountered an error while running the error handler function
	class errfunc_error: public lua::exception {
		errfunc_error(const errfunc_error &) = delete;
		const errfunc_error& operator=(const errfunc_error &) = delete;

	public:
		errfunc_error(state *L)
			: lua::exception(L)
		{}

		errfunc_error(errfunc_error &&other)
			: lua::exception(std::move(other))
		{}
	};

	// thrown by check* functions when they detect an invalid argument
	class check_error: public std::runtime_error {
	public:
		check_error(const std::string &msg)
			: std::runtime_error(msg)
		{}
	};

	// format the string in a form that can be safely read back by the lua interpreter
	// tries to make the string a bit nicer than the lua's %q format specifier
	std::string quote(const std::string &str);

	// a fancy wrapper around lua_State
	class state: private std::mutex {
		lua_State *cobj;

		// destructor for C++ objects stored as lua userdata
		template<typename T>
		static int destroy_cpp_object(lua_State *l)
		{
			T *ptr = static_cast<T *>(lua_touserdata(l, -1));
			assert(ptr);
			try {
				// throwing exceptions in destructors is a bad idea
				// but we catch (and ignore) them, just in case
				ptr->~T();
			}
			catch(...) {
			}
			return 0;
		}

		bool safe_compare(lua_CFunction trampoline, int index1, int index2);
		void throw_check_error(int narg, Type expected) throw(lua::check_error) __attribute__((noreturn));

		/**
		 * The pointed-to value is true if this object still exists. We need this because the
		 * exceptions have to know if they may reference it to remove the saved lua exception. If
		 * this object is destroyed then the exception was already collected by the garbage
		 * colletor and referencing this would generate a segfault.
		 */
		std::shared_ptr<bool> valid;

	public:
		state();
		~state() { *valid = false; lua_close(cobj); }

		std::shared_ptr<const bool> get_valid() const { return valid; }

		/*
		 * Lua functions come in three flavours
		 * a) functions that never throw an exception
		 * b) functions that throw only in case of a memory allocation error
		 * c) functions that throw other kinds of errors
		 *
		 * Calls to type a functions are simply forwarded to the C api.
		 * Type c functions are executed in protected mode, to make sure they don't longjmp()
		 * over us (and our destructors). This add a certain amount overhead. If you care about
		 * performance, try using the raw versions (if possible).
		 * Type b functions are not executed in protected mode atm. as memory allocation errors
		 * don't happen that often (as opposed to the type c, where the user get deliberately set
		 * a metamethod that throws an error). That means those errors will do something
		 * undefined, but hopefully that won't be a problem.
		 *
		 * Semantics are mostly identical to those of the underlying C api. Any deviation is
		 * noted in the respective functions comment. The most important difference is that
		 * instead of return values, we use exceptions to indicate errors.	The lua and C++
		 * exception mechanisms are integrated. That means one can throw a C++ exception and
		 * catch it in lua (with pcall). Lua error()s can be caught in C++ as exceptions of type
		 * lua::exception.
		 */

		// type a, never throw
		int absindex(int index) throw() { return index<0 && -index<=gettop() ? gettop()+1+index : index; }
		bool getmetatable(int index) throw() { return lua_getmetatable(cobj, index); }
		int gettop() throw() { return lua_gettop(cobj); }
		void insert(int index) throw() { lua_insert(cobj, index); }
		bool isboolean(int index) throw() { return lua_isboolean(cobj, index); }
		bool isfunction(int index) throw() { return lua_isfunction(cobj, index); }
		bool islightuserdata(int index) throw() { return lua_islightuserdata(cobj, index); }
		bool isnil(int index) throw() { return lua_isnil(cobj, index); }
		bool isnone(int index) throw() { return lua_isnone(cobj, index); }
		bool isnumber(int index) throw() { return lua_isnumber(cobj, index); }
		bool isstring(int index) throw() { return lua_isstring(cobj, index); }
		bool istable(int index) throw() { return lua_istable(cobj, index); }
		bool isuserdata(int index) throw() { return lua_isuserdata(cobj, index); }
		void pop(int n = 1) throw() { lua_pop(cobj, n); }
		void pushboolean(bool b) throw() { lua_pushboolean(cobj, b); }
		void pushinteger(integer n) throw() { lua_pushinteger(cobj, n); }
		void pushlightuserdata(void *p) throw() { lua_pushlightuserdata(cobj, p); }
		void pushnil() throw() { lua_pushnil(cobj); }
		void pushnumber(number n) throw() { lua_pushnumber(cobj, n); }
		void pushvalue(int index) throw() { lua_pushvalue(cobj, index); }
		void rawget(int index) throw() { lua_rawget(cobj, index); }
		void rawgeti(int index, int n) throw() { lua_rawgeti(cobj, index, n); }
		bool rawequal(int index1, int index2) throw() { return lua_rawequal(cobj, index1, index2); }
		void replace(int index) throw() { lua_replace(cobj, index); }
		// lua_setmetatable returns int, but docs don't specify it's meaning :/
		int setmetatable(int index) throw() { return lua_setmetatable(cobj, index); }
		void settop(int index) throw() { return lua_settop(cobj, index); }
		bool toboolean(int index) throw() { return lua_toboolean(cobj, index); }
		integer tointeger(int index) throw() { return lua_tointeger(cobj, index); }
		number tonumber(int index) throw() { return lua_tonumber(cobj, index); }
		void* touserdata(int index) throw() { return lua_touserdata(cobj, index); }
		Type type(int index) throw() { return static_cast<Type>(lua_type(cobj, index)); }
		// typename is a reserved word :/
		const char* type_name(Type tp) throw() { return lua_typename(cobj, tp); }
		void unref(int t, int ref) throw() { return luaL_unref(cobj, t, ref); }

		// type b, throw only on memory allocation errors
		// checkstack correctly throws bad_alloc, because lua_checkstack kindly informs us of
		// that sitution
		void checkstack(int extra) throw(std::bad_alloc);
		void createtable(int narr = 0, int nrec = 0) { lua_createtable(cobj, narr, nrec); }
		const char* gsub(const char *s, const char *p, const char *r) { return luaL_gsub(cobj, s, p, r); }
		bool newmetatable(const char *tname) { return luaL_newmetatable(cobj, tname); }
		void newtable() { lua_newtable(cobj); }
		void *newuserdata(size_t size) { return lua_newuserdata(cobj, size); }
		// cpp_function can be anything that std::function can handle, everything else remains
		// identical
		void pushclosure(const cpp_function &fn, int n);
		void pushfunction(const cpp_function &fn) { pushclosure(fn, 0); }
		void pushstring(const char *s) { lua_pushstring(cobj, s); }
		void pushstring(const char *s, size_t len) { lua_pushlstring(cobj, s, len); }
		void pushstring(const std::string &s) { lua_pushlstring(cobj, s.c_str(), s.size()); }
		void rawgetfield(int index, const char *k) throw(std::bad_alloc);
		void rawset(int index) { lua_rawset(cobj, index); }
		void rawsetfield(int index, const char *k) throw(std::bad_alloc);
		void rawseti(int index, int n) { lua_rawseti(cobj, index, n); }
		int ref(int t) { return luaL_ref(cobj, t); }
		// len recieves length, if not null. Returned value may contain '\0'
		const char* tocstring(int index, size_t *len = NULL) { return lua_tolstring(cobj, index, len); }
		// Don't use pushclosure() to create a __gc function. The problem is that lua calls them
		// in an unspecified order, and we may end up destroying the object holding the
		// std::function before we get a chance to call it. This pushes a function that simply
		// calls ~T when the time comes. Only set it as __gc on userdata of type T.
		template<typename T>
		void pushdestructor()
		{ lua_pushcfunction(cobj, &destroy_cpp_object<T>); }

		// type c, throw everything but the kitchen sink
		// call() is a protected mode call, we don't allow unprotected calls
		void call(int nargs, int nresults, int errfunc = 0);
		void checkargno(int argno) throw(lua::check_error);
		std::string checkstring(int narg) throw(lua::check_error, std::bad_alloc);
		void *checkudata(int narg, const char *tname) throw(lua::check_error, std::bad_alloc);
		template<typename T>
		T *checkudata(int narg, const char *tname) throw(lua::check_error, std::bad_alloc) { return static_cast<T *>(checkudata(narg, tname)); }
		void concat(int n);
		bool equal(int index1, int index2);
		int gc(int what, int data);
		void getfield(int index, const char *k);
		void gettable(int index);
		void getglobal(const char *name) { getfield(GLOBALSINDEX, name); }
		bool lessthan(int index1, int index2);
		void loadfile(const char *filename) throw(lua::syntax_error, lua::file_error, std::bad_alloc);
		void loadstring(const char *s, const char *chunkname = NULL) throw(lua::syntax_error, std::bad_alloc) { loadstring(s, strlen(s), chunkname); }
		void loadstring(const char *s, size_t len, const char *chunkname = NULL) throw(lua::syntax_error, std::bad_alloc);
		void loadstring(const std::string &s, const char *chunkname = NULL) throw(lua::syntax_error, std::bad_alloc) { loadstring(s.c_str(), s.length(), chunkname); }
		bool next(int index);
		// register is a reserved word :/
		void register_fn(const char *name, const cpp_function &f) { pushfunction(f); setglobal(name); }
		void setfield(int index, const char *k);
		void setglobal(const char *name) { setfield(GLOBALSINDEX, name); }
		void settable(int index);
		// lua_tostring uses NULL to indicate conversion error, since there is no such thing as a
		// NULL std::string, we throw an exception. Returned value may contain '\0'
		std::string tostring(int index) throw(lua::not_string_error);
		// allocate a new lua userdata of appropriate size, and create a object in it
		// pushes the userdata on stack and returns the pointer
		template<typename T, typename... Args>
		T* createuserdata(Args&&... args);

		using std::mutex::lock;
		using std::mutex::unlock;
		using std::mutex::try_lock;
	};

	/*
	 * Can be used to automatically pop temporary values off the lua stack on exit from the
	 * function/block (e.g. via an exception). It's destructor makes sure the stack contains
	 * exactly n items. The constructor initializes n to l.gettop()+n_, but that can be later
	 * changed with the overloaded operators. It is an error if stack contains less than n
	 * elements at entry into the destructor.
	 *
	 * Proposed stack discipline for functions is this:
	 * - called function always pops parameters off the stack.
	 * - if functions returns normally, it's return values are on the stack.
	 * - if function throws an exception, there are no return values on the stack.
	 * The last point differs from lua C api, which return an error message on the stack. But
	 * since we have exception.what() for that, putting the message on the stack is not
	 * necessary.
	 */
	class stack_sentry {
		state *L;
		int n;
	
		stack_sentry(const stack_sentry &) = delete;
		const stack_sentry& operator=(const stack_sentry &) = delete;
	public:
		explicit stack_sentry(state &l, int n_ = 0) throw()
			: L(&l), n(l.gettop()+n_)
		{ assert(n >= 0); }

		~stack_sentry()			throw() { assert(L->gettop() >= n); L->settop(n); }

		void operator++()		throw() { ++n; }
		void operator--()		throw() { --n; assert(n >= 0); }
		void operator+=(int n_) throw() { n+=n_; }
		void operator-=(int n_) throw() { n-=n_; assert(n >= 0); }
	};

	template<typename T, typename... Args>
	T* state::createuserdata(Args&&... args)
	{
		stack_sentry s(*this);

		void *t = newuserdata(sizeof(T));
		new(t) T(std::forward<Args>(args)...);
		++s;
		return static_cast<T *>(t);
	}
}

#endif /* LUAMM_HH */
