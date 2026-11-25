/* Settings of the graphics layer (interface of totk-pc-recomp's cfg.h, MIT),
 * read from environment variables: SW_<NAME> first, then TOTK_<NAME>.  The
 * defaults table is in ../sw_cfg.c. */
#pragma once
const char *cfg_get(const char *name);          /* NULL if unset (name with or without the TOTK_ prefix) */
const char *cfg_value(const char *name);        /* set value, else the table default ("" = none) */
int cfg_int(const char *name);
double cfg_double(const char *name);
int cfg_bool(const char *name);                 /* "0" / "" false, anything else true */
typedef struct { const char *key, *def, *values; int live; } CfgMeta;
const CfgMeta *cfg_meta(const char *key);
const CfgMeta *cfg_meta_at(int i);
void cfg_set(const char *name, const char *value);   /* live settings: not stored in this port */
unsigned cfg_version(void);
int cfg_save(void);
