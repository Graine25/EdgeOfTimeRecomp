#pragma once

namespace eot::debug {

void FreecamTick();

bool FreecamActive();

bool FreecamReadout(float &x, float &y, float &z, float &yaw_degrees, float &pitch_degrees,
                    double &speed);

}
