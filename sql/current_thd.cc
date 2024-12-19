/* Copyright (c) 2015, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/current_thd.h"

/*
1. 线程隔离：使用 thread_local 声明的变量在每个线程中都有独立的存储空间。
   一个线程对该变量的修改不会影响其他线程中的同名变量。这对于多线程编程非常重要，
   因为它可以避免数据竞争和不一致性。
2. 性能优化：由于每个线程都有自己的变量实例，thread_local 可以减少对锁的需求，从而提高性能。
   线程可以独立地访问和修改自己的数据，而不需要频繁地获取和释放锁。
3. 简化代码：使用 thread_local 可以简化多线程代码的设计，
   避免了复杂的同步机制，使得代码更易于理解和维护。
*/
/*
thread_local 保存在每个线程私有栈虚拟内存区域，线程独占
*/
thread_local THD *current_thd = nullptr; // 线程局部存储的当前THD指针，初始化为nullptr

#if defined(_WIN32)
extern "C" THD *_current_thd_noinline(void) { return current_thd; }
#endif
