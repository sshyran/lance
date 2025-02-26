//  Copyright 2022 Lance Authors
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "lance/arrow/utils.h"

#include <arrow/builder.h>
#include <arrow/type.h>
#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

#include "lance/arrow/stl.h"
#include "lance/arrow/type.h"
#include "lance/testing/extension_types.h"

using lance::arrow::MergeSchema;
using lance::arrow::ToArray;

TEST_CASE("Merge simple structs") {
  auto a = lance::arrow::ToArray<int32_t>({1, 2, 3}).ValueOrDie();
  auto left = ::arrow::StructArray::Make({a}, {"a"}).ValueOrDie();

  auto b = lance::arrow::ToArray({"One", "Two", "Three"}).ValueOrDie();
  auto right = ::arrow::StructArray::Make({b}, {"b"}).ValueOrDie();

  auto merged = lance::arrow::MergeStructArrays(left, right).ValueOrDie();
  auto expected =
      ::arrow::StructArray::Make({a, b}, std::vector<std::string>({"a", "b"})).ValueOrDie();
  CHECK(merged->Equals(expected));
}

TEST_CASE("Merge nested structs") {
  auto int_builder = std::make_shared<::arrow::Int32Builder>();
  auto struct_builder = std::make_shared<::arrow::StructBuilder>(
      ::arrow::struct_({::arrow::field("x", ::arrow::int32())}),
      ::arrow::default_memory_pool(),
      std::vector<std::shared_ptr<::arrow::ArrayBuilder>>({int_builder}));
  ::arrow::ListBuilder list_builder(::arrow::default_memory_pool(), struct_builder);

  CHECK(list_builder.Append().ok());
  for (int i = 0; i < 5; i++) {
    CHECK(struct_builder->Append().ok());
    CHECK(int_builder->Append(i).ok());
  }
  auto x = list_builder.Finish().ValueOrDie();
  auto points_x = ::arrow::StructArray::Make({x}, {"points"}).ValueOrDie();

  int_builder->Reset();
  auto point_y_builder = std::make_shared<::arrow::StructBuilder>(
      ::arrow::struct_({::arrow::field("y", ::arrow::int32())}),
      ::arrow::default_memory_pool(),
      std::vector<std::shared_ptr<::arrow::ArrayBuilder>>({int_builder}));
  ::arrow::ListBuilder points_y_builder(::arrow::default_memory_pool(), point_y_builder);

  CHECK(points_y_builder.Append().ok());
  for (int i = 0; i < 5; i++) {
    CHECK(point_y_builder->Append().ok());
    CHECK(int_builder->Append(i * 2).ok());
  }

  auto y = points_y_builder.Finish().ValueOrDie();
  auto points_y = ::arrow::StructArray::Make({y}, {"points"}).ValueOrDie();

  auto points = lance::arrow::MergeStructArrays(points_x, points_y).ValueOrDie();
  INFO("Points array: " << points->ToString());
  auto expected_schema = ::arrow::struct_(
      {::arrow::field("points",
                      arrow::list(::arrow::struct_({::arrow::field("x", ::arrow::int32()),
                                                    ::arrow::field("y", ::arrow::int32())})))});
  INFO("Actual schema: " << points->struct_type()->ToString()
                         << " Expected: " << expected_schema->ToString());
  CHECK(points->struct_type()->Equals(expected_schema));

  auto x_values = lance::arrow::ToArray({0, 1, 2, 3, 4}).ValueOrDie();
  auto y_values = lance::arrow::ToArray({0, 2, 4, 6, 8}).ValueOrDie();
  auto points_struct =
      ::arrow::StructArray::Make({x_values, y_values}, std::vector<std::string>({"x", "y"}))
          .ValueOrDie();
  CHECK(points_struct->length() == 5);
  auto offsets = lance::arrow::ToArray({0, 5}).ValueOrDie();
  auto points_list =
      ::arrow::ListArray::FromArrays(*offsets, *points_struct, ::arrow::default_memory_pool())
          .ValueOrDie();
  auto expected_arr = ::arrow::StructArray::Make({points_list}, {"points"}).ValueOrDie();

  INFO("Actual data: " << points->ToString() << " Expected: " << expected_arr->ToString());
  CHECK(points->Equals(expected_arr));
}

TEST_CASE("Test merge schema") {
  // Merge two primitive fields.
  auto merged = MergeSchema(*::arrow::schema({::arrow::field("a", ::arrow::int32())}),
                            *::arrow::schema({::arrow::field("b", ::arrow::utf8())}))
                    .ValueOrDie();
  INFO("Merged schema: " << merged->ToString());
  CHECK(merged->Equals(::arrow::schema(
      {::arrow::field("a", ::arrow::int32()), {::arrow::field("b", ::arrow::utf8())}})));

  // Merge primitive fields with the same name, but different type.
  auto result = MergeSchema(*::arrow::schema({::arrow::field("a", ::arrow::int32())}),
                            *::arrow::schema({::arrow::field("a", ::arrow::utf8())}));
  CHECK(result.status().IsInvalid());

  // Merge struct<foo:float32> and struct<bar:int64>
  merged =
      MergeSchema(*::arrow::schema({::arrow::field("s",
                                                   ::arrow::struct_({
                                                       ::arrow::field("foo", ::arrow::float32()),
                                                   }))}),
                  *::arrow::schema({::arrow::field("s",
                                                   ::arrow::struct_({
                                                       ::arrow::field("bar", ::arrow::int64()),
                                                   }))}))
          .ValueOrDie();
  CHECK(
      merged->Equals(::arrow::schema({::arrow::field("s",
                                                     ::arrow::struct_({
                                                         ::arrow::field("foo", ::arrow::float32()),
                                                         ::arrow::field("bar", ::arrow::int64()),
                                                     }))})));
}

TEST_CASE("Test merge two lists") {
  // Check "s: list<struct<foo:int32>>" and "s: list<struct<bar:string>>"
  auto merged =
      MergeSchema(
          *::arrow::schema({::arrow::field(
              "s", ::arrow::list(::arrow::struct_({::arrow::field("foo", ::arrow::int32())})))}),
          *::arrow::schema({::arrow::field(
              "s", ::arrow::list(::arrow::struct_({::arrow::field("bar", ::arrow::utf8())})))}))
          .ValueOrDie();
  auto expected = ::arrow::schema(
      {::arrow::field("s",
                      ::arrow::list(::arrow::struct_({::arrow::field("foo", ::arrow::int32()),
                                                      ::arrow::field("bar", ::arrow::utf8())})))});
  INFO("Expected schema: " << expected->ToString() << "\nGot: " << merged->ToString());
  CHECK(merged->Equals(expected));

  // Check "s: large_list<struct<foo:int32>>" and "s: large_list<struct<bar:string>>"
  merged =
      MergeSchema(
          *::arrow::schema({::arrow::field(
              "s",
              ::arrow::large_list(::arrow::struct_({::arrow::field("foo", ::arrow::int32())})))}),
          *::arrow::schema({::arrow::field(
              "s",
              ::arrow::large_list(::arrow::struct_({::arrow::field("bar", ::arrow::utf8())})))}))
          .ValueOrDie();
  expected = ::arrow::schema({::arrow::field(
      "s",
      ::arrow::large_list(::arrow::struct_(
          {::arrow::field("foo", ::arrow::int32()), ::arrow::field("bar", ::arrow::utf8())})))});
  INFO("Expected schema: " << expected->ToString() << "\nGot: " << merged->ToString());
  CHECK(merged->Equals(expected));
}

TEST_CASE("Test merge two fixed size lists") {
  // Merge "s: fixed_size_list<struct<foo:int32>, 4>" and
  // "s: fixed_size_list<struct<bar:string>, 4>"
  auto merged =
      MergeSchema(*::arrow::schema({::arrow::field(
                      "s",
                      ::arrow::fixed_size_list(
                          ::arrow::struct_({::arrow::field("foo", ::arrow::int32())}), 4))}),
                  *::arrow::schema({::arrow::field(
                      "s",
                      ::arrow::fixed_size_list(
                          ::arrow::struct_({::arrow::field("bar", ::arrow::utf8())}), 4))}))
          .ValueOrDie();
  CHECK(merged->Equals(::arrow::schema({::arrow::field(
      "s",
      ::arrow::fixed_size_list(::arrow::struct_({::arrow::field("foo", ::arrow::int32()),
                                                 ::arrow::field("bar", ::arrow::utf8())}),
                               4))})));

  // Merge two different sized lists
  auto result = MergeSchema(
      *::arrow::schema({::arrow::field("l", ::arrow::fixed_size_list(::arrow::int32(), 2))}),
      *::arrow::schema({::arrow::field("l", ::arrow::fixed_size_list(::arrow::int32(), 4))}));
  CHECK(result.status().IsInvalid());

  // Merge two fixed sized list with different types and same length
  result = MergeSchema(
      *::arrow::schema({::arrow::field("l", ::arrow::fixed_size_list(::arrow::utf8(), 4))}),
      *::arrow::schema({::arrow::field("l", ::arrow::fixed_size_list(::arrow::int32(), 4))}));
  CHECK(result.status().IsInvalid());
}

TEST_CASE("Test merge extension types") {
  auto merged = MergeSchema(*::arrow::schema({::arrow::field("box", lance::testing::box2d())}),
                            *::arrow::schema({::arrow::field("box", lance::testing::box2d())}))
                    .ValueOrDie();
  CHECK(merged->Equals(::arrow::schema({::arrow::field("box", lance::testing::box2d())})));

  auto result = MergeSchema(*::arrow::schema({::arrow::field("ann", lance::testing::box2d())}),
                            *::arrow::schema({::arrow::field("ann", lance::testing::image())}));
  CHECK(result.status().IsInvalid());
}