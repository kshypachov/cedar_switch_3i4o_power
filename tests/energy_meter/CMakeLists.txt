cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(energy_meter_test)

# Only the frame check and parse: they have no Zephyr dependency.
target_include_directories(app PRIVATE ../../modules/energy-meter/src)
target_sources(app PRIVATE src/main.c ../../modules/energy-meter/src/hlw8032.c)
