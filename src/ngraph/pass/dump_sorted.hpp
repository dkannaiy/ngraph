// ----------------------------------------------------------------------------
// Copyright 2017 Nervana Systems Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// ----------------------------------------------------------------------------

#pragma once

#include <string>

#include "ngraph/pass/call_pass.hpp"

namespace ngraph
{
    namespace pass
    {
        class DumpSorted;
    }
    class Node;
}

class ngraph::pass::DumpSorted : public CallBase
{
public:
    DumpSorted(const std::string& output_file);

    virtual bool run_on_call_list(std::list<Node*>&) override;

private:
    const std::string m_output_file;
};