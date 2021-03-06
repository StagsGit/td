//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2019
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/utils/Time.h"

#include <cmath>

namespace td {

std::atomic<double> Time::now_;

bool operator==(Timestamp a, Timestamp b) {
  return std::abs(a.at() - b.at()) < 1e-6;
}

}  // namespace td
