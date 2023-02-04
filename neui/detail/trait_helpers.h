#pragma once

/*
	if constexpr(has_member(MyObject,method))
	{
		MyObject* c;
		c->method()
	}

*/

template<typename T, typename F>
constexpr auto has_member_impl(F&& f) -> decltype(f(std::declval<T>()), true)
{
  return true;
}

template<typename>
constexpr bool has_member_impl(...) { return false; }

#define has_member(T, EXPR) \
 has_member_impl<T>( [](auto&& obj)->decltype(obj.EXPR){} )

