/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

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

#ifndef BLOCKS_HTTP_DOCU_SERVER_01_TEST_CC
#define BLOCKS_HTTP_DOCU_SERVER_01_TEST_CC

#include "../../api.h"
#include "../../../../Bricks/strings/printf.h"
#include "../../../../Bricks/dflags/dflags.h"
#include "../../../../3rdparty/gtest/gtest-main-with-dflags.h"

DEFINE_int32(docu_net_server_port_01, PickPortForUnitTest(), "");

using current::strings::Printf;

TEST(Docu, HTTPServer01) {
const auto port = FLAGS_docu_net_server_port_01;
  // Simple "OK" endpoint.
  const auto scope = HTTP(port).Register("/ok", [](Request r) {
    r("OK");
  });
EXPECT_EQ("OK", HTTP(GET(Printf("http://localhost:%d/ok", port))).body);
}

#endif  // BLOCKS_HTTP_DOCU_SERVER_01_TEST_CC
