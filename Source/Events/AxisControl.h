#pragma once

class AxisControl
{
public:
	AxisControl() : amount(0.f) {}

	inline void Add(float amount) { this->amount += amount; }
	inline void Set(float amount) { this->amount = amount; }
	inline float Get() { return amount; }

private:
	float amount;
};