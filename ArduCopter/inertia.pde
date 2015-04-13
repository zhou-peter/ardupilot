/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

// read_inertia - read inertia in from accelerometers
static void read_inertia()
{
    // inertial altitude estimates
    inertial_nav.update(G_Dt);
}

// read_inertial_altitude - pull altitude and climb rate from inertial nav library
static void read_inertial_altitude()
{
    // exit immediately if we do not have an altitude estimate or home is not set
    if (!inertial_nav.get_filter_status().flags.vert_pos || (ap.home_state == HOME_UNSET)) {
        return;
    }

    // with inertial nav we can update the altitude and climb rate at 50hz
    current_loc.alt = inertial_nav.get_alt_above_home_cm();
    current_loc.flags.relative_alt = true;
    climb_rate = inertial_nav.get_velocity_z();
}
