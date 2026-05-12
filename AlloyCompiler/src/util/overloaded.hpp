#pragma once

template<class... Ts>
struct Overloaded : Ts... {
	using Ts::operator()...;
};

// this is a deduction guide which tells the compiler to use the types of the 
// arguments of the constructor as the template arguments
template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;