#pragma once
#include <string>
#include <vpad/input.h>
namespace wiiu::network_keyboard {
void request(const std::string &label,const std::string &initial,unsigned limit,bool secret=false,bool numeric=false);
void input(const VPADStatus &);
bool active();
bool take(std::string &text,bool &accepted);
bool draw(); // GPU thread; tries the native keyboard, false if unavailable.
bool fallback_pending();
void fallback_finish(const std::string &,bool);
std::string label();
std::string initial();
unsigned limit();
bool secret();
bool numeric();
}
