#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace wiiu::network_menu {
struct Row{std::string label,value;bool enabled=true;};
struct View{std::string title,status,hint;std::vector<Row> rows;int selected=0;float scroll=0;};
void open();void close();bool active();
void buttons(uint32_t pressed);void pointer(bool down,float x,float y,float right_y,float seconds);
View view();
}
