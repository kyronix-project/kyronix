#include "common.h"
int main(int argc, char** argv)
{
    if (argc < 2)
        return 0;
    const char* fmt = argv[1];
    int arg = 2;
    for (const char* p = fmt; *p; p++)
    {
        if (*p != '\\' && *p != '%')
        {
            putchar(*p);
            continue;
        }
        if (*p == '\\')
        {
            p++;
            if (*p == 'n')
                putchar('\n');
            else if (*p == 't')
                putchar('\t');
            else if (*p == '\\')
                putchar('\\');
            else if (*p)
                putchar(*p);
            else
                break;
            continue;
        }
        p++;
        if (*p == '%')
            putchar('%');
        else if (*p == 's')
            fputs(arg < argc ? argv[arg++] : "", stdout);
        else if (*p == 'd' || *p == 'i')
            printf("%ld", arg < argc ? strtol(argv[arg++], NULL, 10) : 0L);
        else
        {
            putchar('%');
            if (*p)
                putchar(*p);
        }
    }
    return 0;
}
