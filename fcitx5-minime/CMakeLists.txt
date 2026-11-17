cmake_minimum_required(VERSION 3.16)
project(fcitx5-minime VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)

find_package(Fcitx5Core REQUIRED)

# 插件用到较新的 fcitx5 接口（候选备注、右键菜单、自定义提示、FCITX_ADDON_FACTORY_V2），
# 只支持当前的发行版。逐个探测，缺了就直接报「fcitx5 太旧」，免得出一堆编译错误
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Fcitx5::Core)
set(CMAKE_REQUIRED_FLAGS "-std=c++20")
check_cxx_source_compiles("
#include <fcitx/candidatelist.h>
struct W : fcitx::CandidateWord { W() : CandidateWord(fcitx::Text(\"a\")) { setComment(fcitx::Text(\"b\")); }
  void select(fcitx::InputContext *) const override {} };
int main() { W w; return 0; }" MINIME_HAVE_CANDIDATE_COMMENT)
check_cxx_source_compiles("
#include <fcitx/instance.h>
int main() { fcitx::Instance *i = nullptr; i->showCustomInputMethodInformation(nullptr, \"x\"); return 0; }" MINIME_HAVE_CUSTOM_INFO)
check_cxx_source_compiles("
#include <fcitx/candidateaction.h>
int main() { fcitx::CandidateAction a; a.setId(0); return 0; }" MINIME_HAVE_CANDIDATE_ACTIONS)
check_cxx_source_compiles("
#include <fcitx/addonfactory.h>
#ifndef FCITX_ADDON_FACTORY_V2
#error no v2
#endif
int main() { return 0; }" MINIME_HAVE_FACTORY_V2)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_FLAGS)
foreach(feat MINIME_HAVE_CANDIDATE_COMMENT MINIME_HAVE_CANDIDATE_ACTIONS MINIME_HAVE_CUSTOM_INFO MINIME_HAVE_FACTORY_V2)
  if(NOT ${feat})
    message(FATAL_ERROR "fcitx5 版本太旧（${Fcitx5Core_VERSION}），缺少 ${feat} 对应的接口。"
                        "MinIME 只支持当前发行版里的 fcitx5，请升级系统或 fcitx5。")
  endif()
endforeach()

add_library(minime MODULE src/minime.cpp)
set_target_properties(minime PROPERTIES PREFIX "lib")
target_link_libraries(minime Fcitx5::Core)
target_compile_options(minime PRIVATE -Wall -Wextra)

install(TARGETS minime DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(FILES minime-addon.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon RENAME minime.conf)
install(FILES minime-inputmethod.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod RENAME minime.conf)
