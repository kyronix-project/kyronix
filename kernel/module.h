#pragma once

// module declars here 

#include "version.h"

#define __KMOD_JOIN2(a, b) a##b
#define __KMOD_JOIN(a, b) __KMOD_JOIN2(a, b)
#define MODULE_INFO(tag, value)                                                                    \
    static const char __KMOD_JOIN(__kmod_info_, __LINE__)[]                                        \
        __attribute__((section(".modinfo"), used)) = #tag "=" value

#define MODULE_NAME(value) MODULE_INFO(name, value)
#define MODULE_LICENSE(value) MODULE_INFO(license, value)
#define MODULE_AUTHOR(value) MODULE_INFO(author, value)
#define MODULE_DESCRIPTION(value) MODULE_INFO(description, value)
#define MODULE_DEPENDS(value) MODULE_INFO(depends, value)

MODULE_INFO(vermagic, KERNEL_VERSION);

#define module_init(fn)                                                                            \
    int init_module(void) { return (fn)(); }

#define module_exit(fn)                                                                            \
    void cleanup_module(void) { (fn)(); }

