/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 *       APM_AHRS_DCM.cpp
 *
 *       AHRS system using DCM matrices
 *
 *       Based on DCM code by Doug Weibel, Jordi Mu�oz and Jose Julio. DIYDrones.com
 *
 *       Adapted for the general ArduPilot AHRS interface by Andrew Tridgell

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <AP_AHRS.h>
#include <AP_HAL.h>

extern const AP_HAL::HAL& hal;

// this is the speed in cm/s above which we first get a yaw lock with
// the GPS
#define GPS_SPEED_MIN 300

// this is the speed in cm/s at which we stop using drift correction
// from the GPS and wait for the ground speed to get above GPS_SPEED_MIN
#define GPS_SPEED_RESET 100

// the limit (in degrees/second) beyond which we stop integrating
// omega_I. At larger spin rates the DCM PI controller can get 'dizzy'
// which results in false gyro drift. See
// http://gentlenav.googlecode.com/files/fastRotations.pdf
#define SPIN_RATE_LIMIT 20


// run a full DCM update round
void
AP_AHRS_DCM::update(void)
{
    float delta_t;

    // tell the IMU to grab some data
    _ins.update();

    // ask the IMU how much time this sensor reading represents
    delta_t = _ins.get_delta_time();

    // if the update call took more than 0.2 seconds then discard it,
    // otherwise we may move too far. This happens when arming motors 
    // in ArduCopter
    if (delta_t > 0.2f) {
        memset(&_ra_sum[0], 0, sizeof(_ra_sum));
        _ra_deltat = 0;
        return;
    }

    // Integrate the DCM matrix using gyro inputs
    matrix_update(delta_t);

    // Normalize the DCM matrix
    normalize();

    // Update the earth-frame acceleration
    update_ef_accel(delta_t);

    // Update reference vectors and perform drift correction if necessary
    drift_correction(delta_t);

    // paranoid check for bad values in the DCM matrix
    check_matrix();

    // Calculate pitch, roll, yaw for stabilization and navigation
    euler_angles();

    // update trig values including _cos_roll, cos_pitch
    update_trig();
}

// update the DCM matrix using only the gyros
void
AP_AHRS_DCM::matrix_update(float _G_Dt)
{
    // note that we do not include the P terms in _omega. This is
    // because the spin_rate is calculated from _omega.length(),
    // and including the P terms would give positive feedback into
    // the _P_gain() calculation, which can lead to a very large P
    // value
    _omega.zero();

    // average across all healthy gyros. This reduces noise on systems
    // with more than one gyro    
    uint8_t healthy_count = 0;    
    for (uint8_t i=0; i<_ins.get_gyro_count(); i++) {
        if (_ins.get_gyro_health(i)) {
            _omega += _ins.get_gyro(i);
            healthy_count++;
        }
    }
    if (healthy_count > 1) {
        _omega /= healthy_count;
    }
    _omega += _omega_I;
    _dcm_matrix.rotate((_omega + _omega_P) * _G_Dt);
}


/*
 *  reset the DCM matrix and omega. Used on ground start, and on
 *  extreme errors in the matrix
 */
void
AP_AHRS_DCM::reset(bool recover_eulers)
{
    // reset the integration terms
    _omega_I.zero();
    _omega_P.zero();
    _omega.zero();

    // if the caller wants us to try to recover to the current
    // attitude then calculate the dcm matrix from the current
    // roll/pitch/yaw values
    if (recover_eulers && !isnan(roll) && !isnan(pitch) && !isnan(yaw)) {
        _dcm_matrix.from_euler(roll, pitch, yaw);
    } else {
        // otherwise make it flat
        _dcm_matrix.from_euler(0, 0, 0);
    }
}

// reset the current attitude, used by HIL
void AP_AHRS_DCM::reset_attitude(const float &_roll, const float &_pitch, const float &_yaw)
{
    _dcm_matrix.from_euler(_roll, _pitch, _yaw);    
}

/*
 *  check the DCM matrix for pathological values
 */
void
AP_AHRS_DCM::check_matrix(void)
{
    if (_dcm_matrix.is_nan()) {
        //Serial.printf("ERROR: DCM matrix NAN\n");
        reset(true);
        return;
    }
    // some DCM matrix values can lead to an out of range error in
    // the pitch calculation via asin().  These NaN values can
    // feed back into the rest of the DCM matrix via the
    // error_course value.
    if (!(_dcm_matrix.c.x < 1.0f &&
          _dcm_matrix.c.x > -1.0f)) {
        // We have an invalid matrix. Force a normalisation.
        normalize();

        if (_dcm_matrix.is_nan() ||
            fabsf(_dcm_matrix.c.x) > 10) {
            // normalisation didn't fix the problem! We're
            // in real trouble. All we can do is reset
            //Serial.printf("ERROR: DCM matrix error. _dcm_matrix.c.x=%f\n",
            //	   _dcm_matrix.c.x);
            reset(true);
        }
    }
}

// renormalise one vector component of the DCM matrix
// this will return false if renormalization fails
bool
AP_AHRS_DCM::renorm(Vector3f const &a, Vector3f &result)
{
    float renorm_val;

    // numerical errors will slowly build up over time in DCM,
    // causing inaccuracies. We can keep ahead of those errors
    // using the renormalization technique from the DCM IMU paper
    // (see equations 18 to 21).

    // For APM we don't bother with the taylor expansion
    // optimisation from the paper as on our 2560 CPU the cost of
    // the sqrt() is 44 microseconds, and the small time saving of
    // the taylor expansion is not worth the potential of
    // additional error buildup.

    // Note that we can get significant renormalisation values
    // when we have a larger delta_t due to a glitch eleswhere in
    // APM, such as a I2c timeout or a set of EEPROM writes. While
    // we would like to avoid these if possible, if it does happen
    // we don't want to compound the error by making DCM less
    // accurate.

    renorm_val = 1.0f / a.length();

    // keep the average for reporting
    _renorm_val_sum += renorm_val;
    _renorm_val_count++;

    if (!(renorm_val < 2.0f && renorm_val > 0.5f)) {
        // this is larger than it should get - log it as a warning
        if (!(renorm_val < 1.0e6f && renorm_val > 1.0e-6f)) {
            // we are getting values which are way out of
            // range, we will reset the matrix and hope we
            // can recover our attitude using drift
            // correction before we hit the ground!
            //Serial.printf("ERROR: DCM renormalisation error. renorm_val=%f\n",
            //	   renorm_val);
            return false;
        }
    }

    result = a * renorm_val;
    return true;
}

/*************************************************
 *  Direction Cosine Matrix IMU: Theory
 *  William Premerlani and Paul Bizard
 *
 *  Numerical errors will gradually reduce the orthogonality conditions expressed by equation 5
 *  to approximations rather than identities. In effect, the axes in the two frames of reference no
 *  longer describe a rigid body. Fortunately, numerical error accumulates very slowly, so it is a
 *  simple matter to stay ahead of it.
 *  We call the process of enforcing the orthogonality conditions �renormalization�.
 */
void
AP_AHRS_DCM::normalize(void)
{
    float error;
    Vector3f t0, t1, t2;

    error = _dcm_matrix.a * _dcm_matrix.b;                                              // eq.18

    t0 = _dcm_matrix.a - (_dcm_matrix.b * (0.5f * error));              // eq.19
    t1 = _dcm_matrix.b - (_dcm_matrix.a * (0.5f * error));              // eq.19
    t2 = t0 % t1;                                                       // c= a x b // eq.20

    if (!renorm(t0, _dcm_matrix.a) ||
        !renorm(t1, _dcm_matrix.b) ||
        !renorm(t2, _dcm_matrix.c)) {
        // Our solution is blowing up and we will force back
        // to last euler angles
        reset(true);
    }
}


// produce a yaw error value. The returned value is proportional
// to sin() of the current heading error in earth frame
float
AP_AHRS_DCM::yaw_error_compass(void)
{
    const Vector3f &mag = _compass->get_field();
    // get the mag vector in the earth frame
    Vector2f rb = _dcm_matrix.mulXY(mag);

    rb.normalize();
    if (rb.is_inf()) {
        // not a valid vector
        return 0.0;
    }

    // update vector holding earths magnetic field (if required)
    if( _last_declination != _compass->get_declination() ) {
        _last_declination = _compass->get_declination();
        _mag_earth.x = cosf(_last_declination);
        _mag_earth.y = sinf(_last_declination);
    }

    // calculate the error term in earth frame
    // calculate the Z component of the cross product of rb and _mag_earth
    return rb % _mag_earth;
}

// the _P_gain raises the gain of the PI controller
// when we are spinning fast. See the fastRotations
// paper from Bill.
float
AP_AHRS_DCM::_P_gain(float spin_rate)
{
    if (spin_rate < ToRad(50)) {
        return 1.0f;
    }
    if (spin_rate > ToRad(500)) {
        return 10.0f;
    }
    return spin_rate/ToRad(50);
}

// _yaw_gain reduces the gain of the PI controller applied to heading errors
// when observability from change of velocity is good (eg changing speed or turning)
// This reduces unwanted roll and pitch coupling due to compass errors for planes.
// High levels of noise on _accel_ef will cause the gain to drop and could lead to 
// increased heading drift during straight and level flight, however some gain is
// always available. TODO check the necessity of adding adjustable acc threshold 
// and/or filtering accelerations before getting magnitude
float
AP_AHRS_DCM::_yaw_gain(void) const
{
    float VdotEFmag = pythagorous2(_accel_ef[_active_accel_instance].x,
                                   _accel_ef[_active_accel_instance].y);
    if (VdotEFmag <= 4.0f) {
        return 0.2f*(4.5f - VdotEFmag);
    }
    return 0.1f;
}


// return true if we have and should use GPS
bool AP_AHRS_DCM::have_gps(void) const
{
    if (!_gps || _gps->status() <= GPS::NO_FIX || !_gps_use) {
        return false;
    }
    return true;
}

// return true if we should use the compass for yaw correction
bool AP_AHRS_DCM::use_compass(void)
{
    if (!_compass || !_compass->use_for_yaw()) {
        // no compass available
        return false;
    }
    if (!_flags.fly_forward || !have_gps()) {
        // we don't have any alterative to the compass
        return true;
    }
    if (_gps->ground_speed_cm < GPS_SPEED_MIN) {
        // we are not going fast enough to use the GPS
        return true;
    }

    // if the current yaw differs from the GPS yaw by more than 45
    // degrees and the estimated wind speed is less than 80% of the
    // ground speed, then switch to GPS navigation. This will help
    // prevent flyaways with very bad compass offsets
    int32_t error = abs(wrap_180_cd(yaw_sensor - _gps->ground_course_cd));
    if (error > 4500 && _wind.length() < _gps->ground_speed_cm*0.008f) {
        if (hal.scheduler->millis() - _last_consistent_heading > 2000) {
            // start using the GPS for heading if the compass has been
            // inconsistent with the GPS for 2 seconds
            return false;
        }
    } else {
        _last_consistent_heading = hal.scheduler->millis();
    }

    // use the compass
    return true;    
}


/**
   return an accel vector delayed by AHRS_ACCEL_DELAY samples for a
   specific accelerometer instance
 */
Vector3f AP_AHRS_DCM::ra_delayed(uint8_t instance, const Vector3f &ra)
{
    // get the old element, and then fill it with the new element
    Vector3f ret = _ra_delay_buffer[instance];
    _ra_delay_buffer[instance] = ra;
    return ret;
}

// update _bf_error_compass and _bf_error_compass_time_ms
bool
AP_AHRS_DCM::update_bf_error_compass(void)
{
    if(_gps->last_fix_time - _bf_error_compass_time_ms > 200) {
        _bf_error_compass.zero();
    }
    
    bool new_value = false;
    float yaw_deltat;

    if (use_compass()) {
        /*
          we are using compass for yaw
         */
        if (_compass->last_update != _compass_last_update) {
            yaw_deltat = (_compass->last_update - _compass_last_update) * 1.0e-6f;
            _compass_last_update = _compass->last_update;
            // we force an additional compass read()
            // here. This has the effect of throwing away
            // the first compass value, which can be bad
            if (!_flags.have_initial_yaw && _compass->read()) {
                float heading = _compass->calculate_heading(_dcm_matrix);
                _dcm_matrix.from_euler(roll, pitch, heading);
                _flags.have_initial_yaw = true;
            }
            new_value = true;
            _bf_error_compass = Vector3f(0,0,yaw_error_compass());

            _bf_error_compass_time_ms = _gps->last_fix_time;
        }
    } else if (_flags.fly_forward && have_gps()) {
        /*
          we are using GPS for yaw
         */
        if (_gps->last_fix_time != _bf_error_compass_time_ms &&
            _gps->ground_speed_cm >= GPS_SPEED_MIN) {
            yaw_deltat = (_gps->last_fix_time - _bf_error_compass_time_ms) * 1.0e-3f;
            _bf_error_compass_time_ms = _gps->last_fix_time;
            new_value = true;
            float gps_course_rad = ToRad(_gps->ground_course_cd * 0.01f);
            float yaw_error_rad = wrap_PI(gps_course_rad - yaw);
            _bf_error_compass = Vector3f(0,0,sinf(yaw_error_rad));

            /* reset yaw to match GPS heading under any of the
               following 3 conditions:

               1) if we have reached GPS_SPEED_MIN and have never had
               yaw information before

               2) if the last time we got yaw information from the GPS
               is more than 20 seconds ago, which means we may have
               suffered from considerable gyro drift

               3) if we are over 3*GPS_SPEED_MIN (which means 9m/s)
               and our yaw error is over 60 degrees, which means very
               poor yaw. This can happen on bungee launch when the
               operator pulls back the plane rapidly enough then on
               release the GPS heading changes very rapidly
            */
            if (!_flags.have_initial_yaw || 
                yaw_deltat > 20 ||
                (_gps->ground_speed_cm >= 3*GPS_SPEED_MIN && fabsf(yaw_error_rad) >= 1.047f)) {
                // reset DCM matrix based on current yaw
                _dcm_matrix.from_euler(roll, pitch, gps_course_rad);
                _flags.have_initial_yaw = true;
                _bf_error_compass.zero();
            }
        }
    }
    _error_yaw_sum += fabsf(_bf_error_compass.z);
    _bf_error_compass = _dcm_matrix.mul_transpose(_bf_error_compass);
    _error_yaw_count++;
    
    return new_value;
}

void AP_AHRS_DCM::update_ef_accel(float deltat) {
    // rotate accelerometer values into the earth frame
    for (uint8_t i=0; i<_ins.get_accel_count(); i++) {
        if (_ins.get_accel_health(i)) {
            _accel_ef[i] = _dcm_matrix * _ins.get_accel(i);
            // integrate the accel vector in the earth frame between GPS readings
            _ra_sum[i] += _accel_ef[i] * deltat;
        }
    }

    // keep a sum of the deltat values, so we know how much time
    // we have integrated over
    _ra_deltat += deltat;
}

// update _bf_error_gps and _bf_error_gps_time_ms
bool AP_AHRS_DCM::update_bf_error_gps() {
    uint32_t tnow = hal.scheduler->millis();
    
    if(tnow - _bf_error_gps_time_ms > 1100) {
        _bf_error_gps.zero();
    }
    
    Vector3f velocity;
    uint32_t last_correction_time;
    
    if (!have_gps() || 
        _gps->status() < GPS::GPS_OK_FIX_3D || 
        _gps->num_sats < _gps_minsats) {
        // no GPS, or not a good lock. From experience we need at
        // least 6 satellites to get a really reliable velocity number
        // from the GPS.
        //
        // As a fallback we use the fixed wing acceleration correction
        // if we have an airspeed estimate (which we only have if
        // _fly_forward is set), otherwise no correction
        if (_ra_deltat < 0.2f) {
            // not enough time has accumulated
            return false;
        }
        float airspeed;
        if (_airspeed && _airspeed->use()) {
            airspeed = _airspeed->get_airspeed();
        } else {
            airspeed = _last_airspeed;
        }
        // use airspeed to estimate our ground velocity in
        // earth frame by subtracting the wind
        velocity = _dcm_matrix.colx() * airspeed;

        // add in wind estimate
        velocity += _wind;

        last_correction_time = tnow;
        _have_gps_lock = false;
    } else {
        if (_gps->last_fix_time == _bf_error_gps_time_ms) {
            // we don't have a new GPS fix - nothing more to do
            return false;
        }
        velocity = Vector3f(_gps->velocity_north(), _gps->velocity_east(), _gps->velocity_down());
        last_correction_time = _gps->last_fix_time;
        if (_have_gps_lock == false) {
            // if we didn't have GPS lock in the last drift
            // correction interval then set the velocities equal
            _last_velocity = velocity;
        }
        _have_gps_lock = true;

        // keep last airspeed estimate for dead-reckoning purposes
        Vector3f airspeed = velocity - _wind;
        airspeed.z = 0;
        _last_airspeed = airspeed.length();
    }

    if (have_gps()) {
        // use GPS for positioning with any fix, even a 2D fix
        _last_lat = _gps->latitude;
        _last_lng = _gps->longitude;
        _position_offset_north = 0;
        _position_offset_east = 0;

        // once we have a single GPS lock, we can update using
        // dead-reckoning from then on
        _have_position = true;
    } else {
        // update dead-reckoning position estimate
        _position_offset_north += velocity.x * _ra_deltat;
        _position_offset_east  += velocity.y * _ra_deltat;        
    }

    // see if this is our first time through - in which case we
    // just setup the start times and return
    if (_bf_error_gps_time_ms == 0) {
        _bf_error_gps_time_ms = last_correction_time;
        _last_velocity = velocity;
        return false;
    }

    // equation 9: get the corrected acceleration vector in earth frame. Units
    // are m/s/s
    Vector3f GA_e;
    GA_e = Vector3f(0, 0, -1.0f);

    bool using_gps_corrections = false;
    float ra_scale = 1.0f/(_ra_deltat*GRAVITY_MSS);

    if (_flags.correct_centrifugal && (_have_gps_lock || _flags.fly_forward)) {
        float v_scale = gps_gain.get() * ra_scale;
        Vector3f vdelta = (velocity - _last_velocity) * v_scale;
        
        _gps_yaw_obs = yaw_observability(GRAVITY_MSS*pythagorous2(vdelta.x, vdelta.y));
        GA_e += vdelta;
        GA_e.normalize();
        if (GA_e.is_inf()) {
            // wait for some non-zero acceleration information
            return false;
        }
        using_gps_corrections = true;
    } else {
        _gps_yaw_obs = 0;
    }

    // calculate the error term in earth frame.
    // we do this for each available accelerometer then pick the
    // accelerometer that leads to the smallest error term. This takes
    // advantage of the different sample rates on different
    // accelerometers to dramatically reduce the impact of aliasing
    // due to harmonics of vibrations that match closely the sampling
    // rate of our accelerometers. On the Pixhawk we have the LSM303D
    // running at 800Hz and the MPU6000 running at 1kHz, by combining
    // the two the effects of aliasing are greatly reduced.
    Vector3f error[INS_MAX_INSTANCES];
    Vector3f GA_b[INS_MAX_INSTANCES];
    int8_t besti = -1;
    float best_error = 0;
    for (uint8_t i=0; i<_ins.get_accel_count(); i++) {
        if (!_ins.get_accel_health()) {
            // only use healthy sensors
            continue;
        }
        _ra_sum[i] *= ra_scale;

        // get the delayed ra_sum to match the GPS lag
        if (using_gps_corrections) {
            GA_b[i] = ra_delayed(i, _ra_sum[i]);
        } else {
            GA_b[i] = _ra_sum[i];
        }
        
        if (GA_b[i].is_inf()) {
            // wait for some non-zero acceleration information
            continue;
        }
        error[i] = GA_b[i] % GA_e;
        
        float error_length = error[i].length();
        if (besti == -1 || error_length < best_error) {
            besti = i;
            best_error = error_length;
        }
        
        float GA_b_length = GA_b[i].length();
        if(GA_b_length > 1.0f) {
            error[i] /= GA_b_length;
        }
    }
    
    if (besti == -1) {
        // no healthy accelerometers!
        return false;
    }

    _active_accel_instance = besti;

    if (error[besti].is_nan() || error[besti].is_inf()) {
        // don't allow bad values
        check_matrix();
        return false;
    }

    // if ins is unhealthy then stop attitude drift correction and
    // hope the gyros are OK for a while. Just slowly reduce _omega_P
    // to prevent previous bad accels from throwing us off
    if (!_ins.healthy()) {
        _bf_error_gps = Vector3f();
    } else {
        _bf_error_gps.z *= _gps_yaw_obs;
        // convert the error term to body frame
        _bf_error_gps = _dcm_matrix.mul_transpose(error[besti]);
    }

    _error_rp_sum += best_error;
    _error_rp_count++;

    // zero our accumulator ready for the next GPS step
    memset(&_ra_sum[0], 0, sizeof(_ra_sum));
    _ra_deltat = 0;
    _bf_error_gps_time_ms = last_correction_time;

    // remember the velocity for next time
    _last_velocity = velocity;
    return true;
}

float AP_AHRS_DCM::yaw_observability(float lateral_accel) {
    if(lateral_accel < 1 || lateral_accel > 20) {
        return 0.0f;
    } else if(lateral_accel <= 3) {
        return lateral_accel*0.5-0.5;
    } else if(lateral_accel <= 10) {
        return 1.0f;
    } else if(lateral_accel <= 20) {
        return -lateral_accel*.1+2;
    }
    return 0.0f;
}

// perform drift correction. This function aims to update _omega_P and
// _omega_I with our best estimate of the short term and long term
// gyro error. The _omega_P value is what pulls our attitude solution
// back towards the reference vector quickly. The _omega_I term is an
// attempt to learn the long term drift rate of the gyros.
//
// This drift correction implementation is based on a paper
// by Bill Premerlani from here:
//   http://gentlenav.googlecode.com/files/RollPitchDriftCompensation.pdf
void
AP_AHRS_DCM::drift_correction(float deltat)
{
    float spin_rate = _omega.length();
    
    if(update_bf_error_gps() || update_bf_error_compass()) {
        _omega_P = _bf_error_gps * _P_gain(spin_rate) * _kp;
        _omega_P += _bf_error_compass * (1.0f-_gps_yaw_obs) * _P_gain(spin_rate) * _kp_yaw;
        
        if (_flags.fast_ground_gains) {
            _omega_P *= 8;
        }

        if (_flags.fly_forward && _gps && _gps->status() >= GPS::GPS_OK_FIX_2D && 
            _gps->ground_speed_cm < GPS_SPEED_MIN && 
            _ins.get_accel().x >= 7 &&
            pitch_sensor > -3000 && pitch_sensor < 3000) {
                // assume we are in a launch acceleration, and reduce the
                // rp gain by 50% to reduce the impact of GPS lag on
                // takeoff attitude when using a catapult
                _omega_P *= 0.5f;
        }
    }

    // accumulate some integrator error
    if (spin_rate < ToRad(SPIN_RATE_LIMIT)) {
        Vector3f error = _bf_error_gps + _bf_error_compass*(1.0f-_gps_yaw_obs);
        _omega_I_sum += error * _ki * deltat;
        _omega_I_sum_time += deltat;
    }

    if (_omega_I_sum_time >= 5) {
        // limit the rate of change of omega_I to the hardware
        // reported maximum gyro drift rate. This ensures that
        // short term errors don't cause a buildup of omega_I
        // beyond the physical limits of the device
        float change_limit = _gyro_drift_limit * _omega_I_sum_time;
        _omega_I_sum.x = constrain_float(_omega_I_sum.x, -change_limit, change_limit);
        _omega_I_sum.y = constrain_float(_omega_I_sum.y, -change_limit, change_limit);
        _omega_I_sum.z = constrain_float(_omega_I_sum.z, -change_limit, change_limit);
        _omega_I += _omega_I_sum;
        _omega_I_sum.zero();
        _omega_I_sum_time = 0;
    }
}


// update our wind speed estimate
void AP_AHRS_DCM::estimate_wind(void)
{
    if (!_flags.wind_estimation) {
        return;
    }
    const Vector3f &velocity = _last_velocity;

    // this is based on the wind speed estimation code from MatrixPilot by
    // Bill Premerlani. Adaption for ArduPilot by Jon Challinger
    // See http://gentlenav.googlecode.com/files/WindEstimation.pdf
    Vector3f fuselageDirection = _dcm_matrix.colx();
    Vector3f fuselageDirectionDiff = fuselageDirection - _last_fuse;
    uint32_t now = hal.scheduler->millis();

    // scrap our data and start over if we're taking too long to get a direction change
    if (now - _last_wind_time > 10000) {
        _last_wind_time = now;
        _last_fuse = fuselageDirection;
        _last_vel = velocity;
        return;
    }

    float diff_length = fuselageDirectionDiff.length();
    if (diff_length > 0.2f) {
        // when turning, use the attitude response to estimate
        // wind speed
        float V;
        Vector3f velocityDiff = velocity - _last_vel;

        // estimate airspeed it using equation 6
        V = velocityDiff.length() / diff_length;

        _last_fuse = fuselageDirection;
        _last_vel = velocity;

        Vector3f fuselageDirectionSum = fuselageDirection + _last_fuse;
        Vector3f velocitySum = velocity + _last_vel;

        float theta = atan2f(velocityDiff.y, velocityDiff.x) - atan2f(fuselageDirectionDiff.y, fuselageDirectionDiff.x);
        float sintheta = sinf(theta);
        float costheta = cosf(theta);

        Vector3f wind = Vector3f();
        wind.x = velocitySum.x - V * (costheta * fuselageDirectionSum.x - sintheta * fuselageDirectionSum.y);
        wind.y = velocitySum.y - V * (sintheta * fuselageDirectionSum.x + costheta * fuselageDirectionSum.y);
        wind.z = velocitySum.z - V * fuselageDirectionSum.z;
        wind *= 0.5f;

        if (wind.length() < _wind.length() + 20) {
            _wind = _wind * 0.95f + wind * 0.05f;
        }

        _last_wind_time = now;
    } else if (now - _last_wind_time > 2000 && _airspeed && _airspeed->use()) {
        // when flying straight use airspeed to get wind estimate if available
        Vector3f airspeed = _dcm_matrix.colx() * _airspeed->get_airspeed();
        Vector3f wind = velocity - (airspeed * get_EAS2TAS());
        _wind = _wind * 0.92f + wind * 0.08f;
    }    
}



// calculate the euler angles and DCM matrix which will be used for high level
// navigation control. Apply trim such that a positive trim value results in a 
// positive vehicle rotation about that axis (ie a negative offset)
void
AP_AHRS_DCM::euler_angles(void)
{
    _body_dcm_matrix = _dcm_matrix;
    _body_dcm_matrix.rotateXYinv(_trim);
    _body_dcm_matrix.to_euler(&roll, &pitch, &yaw);

    roll_sensor     = degrees(roll)  * 100;
    pitch_sensor    = degrees(pitch) * 100;
    yaw_sensor      = degrees(yaw)   * 100;

    if (yaw_sensor < 0)
        yaw_sensor += 36000;
}

/* reporting of DCM state for MAVLink */

// average error_roll_pitch since last call
float AP_AHRS_DCM::get_error_rp(void)
{
    if (_error_rp_count == 0) {
        // this happens when telemetry is setup on two
        // serial ports
        return _error_rp_last;
    }
    _error_rp_last = _error_rp_sum / _error_rp_count;
    _error_rp_sum = 0;
    _error_rp_count = 0;
    return _error_rp_last;
}

// average error_yaw since last call
float AP_AHRS_DCM::get_error_yaw(void)
{
    if (_error_yaw_count == 0) {
        // this happens when telemetry is setup on two
        // serial ports
        return _error_yaw_last;
    }
    _error_yaw_last = _error_yaw_sum / _error_yaw_count;
    _error_yaw_sum = 0;
    _error_yaw_count = 0;
    return _error_yaw_last;
}

// return our current position estimate using
// dead-reckoning or GPS
bool AP_AHRS_DCM::get_position(struct Location &loc)
{
    loc.lat = _last_lat;
    loc.lng = _last_lng;
    loc.alt = _baro.get_altitude() * 100 + _home.alt;
    location_offset(loc, _position_offset_north, _position_offset_east);
    if (_flags.fly_forward && _have_position) {
        location_update(loc, degrees(yaw), _gps->ground_speed_cm * 0.01 * _gps->get_lag());
    }
    return _have_position;
}

// return an airspeed estimate if available
bool AP_AHRS_DCM::airspeed_estimate(float *airspeed_ret) const
{
	bool ret = false;
	if (_airspeed && _airspeed->use()) {
		*airspeed_ret = _airspeed->get_airspeed();
		return true;
	}

    if (!_flags.wind_estimation) {
        return false;
    }

	// estimate it via GPS speed and wind
	if (have_gps()) {
		*airspeed_ret = _last_airspeed;
		ret = true;
	}

	if (ret && _wind_max > 0 && _gps && _gps->status() >= GPS::GPS_OK_FIX_2D) {
		// constrain the airspeed by the ground speed
		// and AHRS_WIND_MAX
        float gnd_speed = _gps->ground_speed_cm*0.01f;
        float true_airspeed = *airspeed_ret * get_EAS2TAS();
		true_airspeed = constrain_float(true_airspeed,
                                        gnd_speed - _wind_max, 
                                        gnd_speed + _wind_max);
        *airspeed_ret = true_airspeed / get_EAS2TAS();
	}
	return ret;
}

void AP_AHRS_DCM::set_home(int32_t lat, int32_t lng, int32_t alt_cm)
{
    _home.lat = lat;
    _home.lng = lng;
    _home.alt = alt_cm;
}

