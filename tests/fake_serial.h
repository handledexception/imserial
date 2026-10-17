#pragma once
#include "serial/serial.h"

void fake_plug(int dev, const char *id, const char *path);
void fake_unplug(int dev);
void fake_inject(const char *data);
size_t fake_tx_total(void);
