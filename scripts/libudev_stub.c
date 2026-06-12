/* libudev.so.1 stub — Xorg calls udev_new() first; NULL return disables udev path */
#include <stddef.h>
#include <stdint.h>

#define E __attribute__((visibility("default")))

E void* udev_new(void)
{
    return NULL;
}
E void udev_unref(void* u)
{
    (void) u;
}

E void* udev_monitor_new_from_netlink(void* u, const char* s)
{
    (void) u;
    (void) s;
    return NULL;
}
E int udev_monitor_filter_add_match_subsystem_devtype(void* m, const char* s, const char* t)
{
    (void) m;
    (void) s;
    (void) t;
    return -1;
}
E int udev_monitor_enable_receiving(void* m)
{
    (void) m;
    return -1;
}
E int udev_monitor_get_fd(void* m)
{
    (void) m;
    return -1;
}
E void* udev_monitor_get_udev(void* m)
{
    (void) m;
    return NULL;
}
E void* udev_monitor_receive_device(void* m)
{
    (void) m;
    return NULL;
}
E void udev_monitor_unref(void* m)
{
    (void) m;
}

E void* udev_enumerate_new(void* u)
{
    (void) u;
    return NULL;
}
E int udev_enumerate_add_match_subsystem(void* e, const char* s)
{
    (void) e;
    (void) s;
    return 0;
}
E int udev_enumerate_add_match_sysname(void* e, const char* s)
{
    (void) e;
    (void) s;
    return 0;
}
E int udev_enumerate_scan_devices(void* e)
{
    (void) e;
    return 0;
}
E void* udev_enumerate_get_list_entry(void* e)
{
    (void) e;
    return NULL;
}
E void udev_enumerate_unref(void* e)
{
    (void) e;
}

E void* udev_list_entry_get_next(void* l)
{
    (void) l;
    return NULL;
}
E const char* udev_list_entry_get_name(void* l)
{
    (void) l;
    return NULL;
}
E const char* udev_list_entry_get_value(void* l)
{
    (void) l;
    return NULL;
}

E void* udev_device_new_from_syspath(void* u, const char* p)
{
    (void) u;
    (void) p;
    return NULL;
}
E void* udev_device_get_parent(void* d)
{
    (void) d;
    return NULL;
}
E void udev_device_unref(void* d)
{
    (void) d;
}
E const char* udev_device_get_devnode(void* d)
{
    (void) d;
    return NULL;
}
E const char* udev_device_get_subsystem(void* d)
{
    (void) d;
    return NULL;
}
E const char* udev_device_get_devtype(void* d)
{
    (void) d;
    return NULL;
}
E const char* udev_device_get_syspath(void* d)
{
    (void) d;
    return NULL;
}
E const char* udev_device_get_sysname(void* d)
{
    (void) d;
    return NULL;
}
E const char* udev_device_get_property_value(void* d, const char* k)
{
    (void) d;
    (void) k;
    return NULL;
}
E const char* udev_device_get_sysattr_value(void* d, const char* k)
{
    (void) d;
    (void) k;
    return NULL;
}
E int udev_device_get_is_initialized(void* d)
{
    (void) d;
    return 1;
}
E const char* udev_device_get_action(void* d)
{
    (void) d;
    return NULL;
}
E uint64_t udev_device_get_devnum(void* d)
{
    (void) d;
    return 0;
}
E void* udev_device_get_properties_list_entry(void* d)
{
    (void) d;
    return NULL;
}
