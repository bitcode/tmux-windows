#ifndef _LANGINFO_H
#define _LANGINFO_H

#define CODESET 0

static inline char *nl_langinfo(int item)
{
    (void)item;
    return "UTF-8";
}

#endif
