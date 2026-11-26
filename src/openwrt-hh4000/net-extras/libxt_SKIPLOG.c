/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <xtables.h>

static void skiplog_help(void)
{
 puts("SKIPLOG: exclude the matching packet from Broadcom flow learning.");
}

static struct xtables_target skiplog_target = {
 .version = XTABLES_VERSION,
 .name = "SKIPLOG",
 .revision = 0,
 .family = NFPROTO_UNSPEC,
 .help = skiplog_help,
};

static void __attribute__((constructor)) register_skiplog(void)
{
 xtables_register_target(&skiplog_target);
}
