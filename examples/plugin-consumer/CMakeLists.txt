cmake_minimum_required(VERSION 3.20)
project(gygax_plugin_consumer LANGUAGES CXX)

find_package(Gygax 0.2 REQUIRED)

gygax_add_plugin(fleet_plugin SOURCES fleet_plugin.cpp)
