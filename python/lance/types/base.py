#  Copyright (c) 2022. Lance Developers
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
import platform
from abc import ABC

import pyarrow as pa


class LanceType(pa.ExtensionType, ABC):
    def __init__(self, storage_type, extension_name):
        super(LanceType, self).__init__(storage_type, extension_name)


class Point2dType(LanceType):
    """
    A Point in 2D space. Represented as 2 floating point numbers
    """

    def __init__(self):
        super(Point2dType, self).__init__(
            pa.list_(pa.float64(), list_size=2),
            "point2d",
        )

    def __arrow_ext_serialize__(self):
        return b""

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return Point2dType()