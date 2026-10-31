#pragma once

namespace tsukuyomi::customguiscale {

int ceilHalf(int mx);
int defaultIndex(int d, int mx);
float customScale(float base, float maximum, float minimum, int percent);
void percentRange(float base, float maximum, float minimum, int& lo, int& hi);
int clampPercent(int percent, int lo, int hi);

}
