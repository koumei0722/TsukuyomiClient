#pragma once

namespace tsukuyomi::placeamount {

void setNext(int items);
void clear();

bool available();
void markAvailable();

void apply(void* amount);

}
