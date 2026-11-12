#pragma once

#include <array>

// Standard soccar kickoff spawns. Ball sits at (0, 0).
// Yaw is in Unreal rotator units (65536 per full turn).

enum class KickoffType
{
	Corner,
	OffCenter,
	Center
};

struct KickoffSpawn
{
	const char* name;
	float x;
	float y;
	int yaw;
	KickoffType type;
};

inline constexpr std::array<KickoffSpawn, 10> KICKOFF_SPAWNS = { {
	{ "Right corner",  -2048.0f, -2560.0f,   8192, KickoffType::Corner },
	{ "Left corner",    2048.0f, -2560.0f,  24576, KickoffType::Corner },
	{ "Back right",     -256.0f, -3840.0f,  16384, KickoffType::OffCenter },
	{ "Back left",       256.0f, -3840.0f,  16384, KickoffType::OffCenter },
	{ "Back center",       0.0f, -4608.0f,  16384, KickoffType::Center },
	{ "Right corner",   2048.0f,  2560.0f, -24576, KickoffType::Corner },
	{ "Left corner",   -2048.0f,  2560.0f,  -8192, KickoffType::Corner },
	{ "Back right",      256.0f,  3840.0f, -16384, KickoffType::OffCenter },
	{ "Back left",      -256.0f,  3840.0f, -16384, KickoffType::OffCenter },
	{ "Back center",       0.0f,  4608.0f, -16384, KickoffType::Center },
} };

// Distance from each spawn type to the ball (uu)
inline constexpr float KICKOFF_DIST_CORNER = 3278.3f;
inline constexpr float KICKOFF_DIST_OFFCENTER = 3848.5f;
inline constexpr float KICKOFF_DIST_CENTER = 4608.0f;

// Supersonic speed; a speedflipping car is at max speed well before the ball,
// so extra distance costs distance / max speed
inline constexpr float KICKOFF_MAX_SPEED = 2300.0f;

// Number of ticks after a round loads during which the car/ball get snapped
inline constexpr int KICKOFF_PREP_TICKS = 10;

// Ticks after the clock hits zero before forcing the ball to explode
inline constexpr int KICKOFF_EXPLODE_GRACE_TICKS = 12;
