#pragma once

enum class Status : uint8_t
{
	Error = 0,
	Ok
};

template<typename T>
struct Result
{
	Status ok;
	T value;
};
