#ifndef RESTORE_H
#define RESTORE_H

#include "config.h"

/*
 * do_restore  –  Interactieve restore flow.
 *
 * Toont een titelselectiescherm, timestamp-selectiescherm en bevestiging,
 * downloadt de backup en resignt indien non-roamable.
 */
void do_restore(const char *creds64, const AppConfig *cfg);

#endif /* RESTORE_H */
