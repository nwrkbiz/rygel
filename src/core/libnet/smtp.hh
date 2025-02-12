// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "src/core/libcc/libcc.hh"

namespace RG {

struct smtp_Config {
    const char *url = nullptr;
    const char *username = nullptr;
    const char *password = nullptr;
    const char *from = nullptr;

    bool Validate() const;
};

struct smtp_MailContent {
    const char *subject = nullptr;
    const char *text = nullptr;
    const char *html = nullptr;
};

class smtp_Sender {
    smtp_Config config;

    BlockAllocator str_alloc;

public:
    bool Init(const smtp_Config &config);
    bool Send(const char *to, const smtp_MailContent &content);
};

}
