# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Версия прошивки: видна в /settings и на веб-странице. Увеличивайте при каждом обновлении.
set(PROJECT_VER "1.3.3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JkongTelegramm)
