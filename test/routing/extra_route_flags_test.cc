#include "gtest/gtest.h"

#include <sstream>

#include "nigiri/loader/load.h"
#include "nigiri/timetable.h"

#include "../raptor_search.h"

using namespace nigiri;
using namespace date;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace {

// Same timetable as no_compulsory_reservation_test (test_files_1):
// T0, T2, T5 have pickup_type=2 (compulsory reservation),
// T3 -> T5 and T4 -> T6 are stay-seated (block 1) -> flags on some sections.
constexpr auto const kTestFiles = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_desc,stop_lat,stop_lon,stop_url,location_type,parent_station
A,A,,0.0,1.0,,
B,B,,2.0,3.0,,
C,C,,4.0,5.0,,
D,D,,6.0,7.0,,
E,E,,8.0,9.0,,
F,F,,10.0,11.0,,

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
R0,DB,0,,,3
R1,DB,1,,,3
R2,DB,2,,,3
R3,DB,3,,,3
R4,DB,4,,,3
R5,DB,5,,,3

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id,pickup_type
R0,S1,T0,,,2
R1,S1,T1,,,0
R1,S1,T2,,,2
R2,S1,T3,,1,0
R2,S1,T4,,1,0
R3,S1,T5,,1,2
R3,S1,T6,,1,0

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence
T0,10:00:00,10:00:00,A,0
T0,10:10:00,10:10:00,B,1
T0,10:20:00,10:20:00,C,1
T1,10:00:00,10:00:00,A,0
T1,10:15:00,10:15:00,B,1
T1,10:30:00,10:30:00,C,1
T2,11:00:00,11:00:00,A,0
T2,11:15:00,11:15:00,B,1
T2,11:30:00,11:30:00,C,1
T3,10:35:00,10:35:00,C,0
T3,10:40:00,10:40:00,D,1
T4,10:45:00,10:45:00,C,0
T4,10:50:00,10:50:00,D,1
T5,10:42:00,10:42:00,D,0
T5,10:50:00,10:50:00,E,0
T5,10:55:00,10:55:00,F,0
T6,10:52:00,10:52:00,D,0
T6,11:00:00,11:00:00,E,0
T6,11:05:00,11:05:00,F,0

# calendar_dates.txt
service_id,date,exception_type
S1,20190501,1
)"sv;

// Moves the reservation flag to the supplement / pass flags.
constexpr auto const kScript = R"(
function process_trip(trip)
  local reservation = trip:get_compulsory_reservation()
  trip:set_supplement_required(reservation)
  trip:set_included_in_pass(not reservation)
  trip:set_compulsory_reservation(false)
  return true
end
)";

constexpr auto const kAllTrue = std::array<bool, kNumClasses>{
    true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, true};

timetable load(std::string const& script) {
  return loader::load({{loader::mem_dir::read(kTestFiles),
                        {.reservation_not_required_default_ = kAllTrue,
                         .user_script_ = script}}},
                      {2019_y / May / 1, 2019_y / May / 2});
}

struct filters {
  bool no_compulsory_reservation_{false};
  bool no_supplement_required_{false};
  bool only_included_in_pass_{false};
};

std::string search(timetable const& tt,
                   std::string_view from,
                   std::string_view to,
                   direction const dir,
                   filters const f) {
  if (dir == direction::kBackward) {
    std::swap(from, to);
  }
  auto const src = source_idx_t{0};
  auto q = routing::query{
      .start_time_ = tt.date_range_,
      .start_ = {{tt.locations_.location_id_to_idx_.at({from, src}), 0_minutes,
                  0U}},
      .destination_ = {{tt.locations_.location_id_to_idx_.at({to, src}),
                        0_minutes, 0U}},
      .no_compulsory_reservation_ = f.no_compulsory_reservation_,
      .no_supplement_required_ = f.no_supplement_required_,
      .only_included_in_pass_ = f.only_included_in_pass_};
  auto const results = test::raptor_search(tt, nullptr, std::move(q), dir);
  auto ss = std::stringstream{};
  for (auto const& j : results) {
    j.print(ss, tt, nullptr);
    ss << "\n";
  }
  return ss.str();
}

}  // namespace

TEST(routing, extra_route_flags) {
  auto const ref = load("");
  auto const tt = load(kScript);

  constexpr auto const kReservation =
      filters{.no_compulsory_reservation_ = true};
  constexpr auto const kSupplement = filters{.no_supplement_required_ = true};
  constexpr auto const kPass = filters{.only_included_in_pass_ = true};
  constexpr auto const kBoth =
      filters{.no_supplement_required_ = true, .only_included_in_pass_ = true};

  for (auto const dir : {direction::kForward, direction::kBackward}) {
    for (auto const [from, to] : {std::pair{"A"sv, "C"sv},
                                  {"C"sv, "D"sv},
                                  {"A"sv, "D"sv},
                                  {"A"sv, "F"sv}}) {
      SCOPED_TRACE(fmt::format("{} -> {}, {}", from, to,
                               dir == direction::kForward ? "fwd" : "bwd"));

      auto const unfiltered = search(ref, from, to, dir, {});
      auto const filtered = search(ref, from, to, dir, kReservation);

      EXPECT_EQ(unfiltered, search(tt, from, to, dir, {}));
      EXPECT_EQ(unfiltered, search(tt, from, to, dir, kReservation));
      EXPECT_EQ(filtered, search(tt, from, to, dir, kSupplement));
      EXPECT_EQ(filtered, search(tt, from, to, dir, kPass));
      EXPECT_EQ(filtered, search(tt, from, to, dir, kBoth));
    }

    // Make sure the filters actually change something (route level: A->C,
    // section level: A->F via stay-seated T3 -> T5).
    EXPECT_NE(search(ref, "A", "C", dir, {}),
              search(tt, "A", "C", dir, kSupplement));
    EXPECT_NE(search(ref, "A", "F", dir, {}),
              search(tt, "A", "F", dir, kSupplement));
  }
}
