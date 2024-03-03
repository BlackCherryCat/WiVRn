/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "clock_offset.h"

#include "driver/wivrn_connection.h"
#include "os/os_time.h"
#include "util/u_logging.h"

static const size_t num_samples = 100;

void clock_offset_estimator::request_sample(wivrn_connection & connection)
{
	if (std::chrono::steady_clock::now() < next_sample)
		return;

	next_sample = std::chrono::steady_clock::now() + sample_interval.load();
	xrt::drivers::wivrn::to_headset::timesync_query timesync{};
	timesync.query = std::chrono::nanoseconds(os_monotonic_get_ns());
	connection.send_stream(timesync);
}

void clock_offset_estimator::add_sample(const xrt::drivers::wivrn::from_headset::timesync_response & base_sample)
{
	auto now = std::chrono::nanoseconds(os_monotonic_get_ns());
	clock_offset_estimator::sample sample{base_sample, now};
	std::lock_guard lock(mutex);
	if (samples.size() < num_samples)
	{
		samples.push_back(sample);
	}
	else
	{
		sample_interval = std::chrono::seconds(1);
		int64_t latency = 0;
		for (const auto& s: samples)
			latency += (s.received - s.query).count();
		latency /= samples.size();
		// packets with too high latency are likely to be retransmitted
		if ((sample.received - sample.query).count() > 3 * latency)
		{
			U_LOG_D("drop packet for latency %ld > %ld", (sample.received - sample.query).count() /1000, latency/1000);
			return;
		}

		samples[sample_index] = sample;
		sample_index = (sample_index + 1) % num_samples;
	}

	// Linear regression
	// X = time on server
	// Y = time on headset
	// in order to maintain accuracy, use x = X-x0, y = Y-y0
	// where x0 and y0 are means of X an Y
	size_t n = samples.size();
	double inv_n = 1. / n;
	double x0 = 0;
	double y0 = 0;
	for (const auto & s: samples)
	{
		x0 += (s.query + s.received).count() * 0.5;
		y0 += s.response;
	}
	x0 /= n;
	y0 /= n;

	if (samples.size() < num_samples)
	{
		offset.a = 1;
		offset.b = y0 - x0;
		return;
	}

	double sum_x = 0;
	double sum_y = 0;
	double sum_x2 = 0;
	double sum_xy = 0;
	for (const auto & s: samples)
	{
#if 1
		// assume symmetrical latency
		double x = (s.query + s.received).count() * 0.5 - x0;
#else
		// assume latency is only on server -> headset link
		double x = (s.received).count() - x0;
#endif
		double y = s.response - y0;
		sum_x += x;
		sum_y += y;
		sum_x2 += x * x;
		sum_xy += x * y;
	}

	double mean_x = sum_x * inv_n;
	double mean_y = sum_y * inv_n;
	// y = ax + b
	double cov = inv_n * sum_xy - mean_x * mean_y;
	double v = inv_n * sum_x2 - mean_x * mean_x;
	double a = cov / v;
	double b = mean_y - a * mean_x;

	offset.a = a;
	offset.b = y0 + b - int64_t(a * x0);
	U_LOG_D("clock relations: headset = a*x+b where a=%f b=%ldµs", offset.a, offset.b / 1000);
}

clock_offset clock_offset_estimator::get_offset()
{
	std::lock_guard lock(mutex);
	return offset;
}

int64_t clock_offset::from_headset(uint64_t ts) const
{
	int64_t res = (int64_t(ts) - b) / a;
#ifndef NDEBUG
	if (res < 0)
		U_LOG_W("negative from_headset: %ld", res);
#endif
	return res;
}

std::chrono::nanoseconds clock_offset::to_headset(uint64_t timestamp_ns) const
{
	int64_t res = int64_t(a * timestamp_ns) + b;
#ifndef NDEBUG
	if (res < 0)
		U_LOG_W("negative to_headset: %ld", res);
#endif
	return std::chrono::nanoseconds(res);
}
