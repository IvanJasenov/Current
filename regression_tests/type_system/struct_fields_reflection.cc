/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Maxim Zhurovich <zhurovich@gmail.com>
              2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef CURRENT_COVERAGE_REPORT_MODE

#include "../../typesystem/struct.h"
#include "../../typesystem/Schema/schema.h"
#include "../../typesystem/Reflection/reflection.h"

#include "../../bricks/file/file.h"

#include "../../3rdparty/gtest/gtest-main.h"

namespace type_test {

#include "include/struct_fields.h"

}  // namespace type_test

TEST(TypeTest, StructFields) {
  using namespace type_test;

  current::reflection::StructSchema schema;
  schema.AddType<StructWithManyFields>();
  EXPECT_EQ(current::FileSystem::ReadFileAsString("golden/struct_fields.cc"),
            schema.GetSchemaInfo().Describe<current::reflection::Language::CPP>(false));
}

#endif  // CURRENT_COVERAGE_REPORT_MODE
