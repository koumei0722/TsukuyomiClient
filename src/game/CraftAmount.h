#pragma once

namespace tsukuyomi::craftamount {

void setNext(int items);
void clear();

bool available();
void markAvailable();

void apply(void* amount);

}
