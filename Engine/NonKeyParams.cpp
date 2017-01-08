/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NonKeyParams.h"

#include <cassert>
#include <stdexcept>

#include "Serialization/NonKeyParamsSerialization.h"

NATRON_NAMESPACE_ENTER;

NonKeyParams::NonKeyParams()
    : _storageInfo()
{
}

NonKeyParams::NonKeyParams(const CacheEntryStorageInfo& info)
    : _storageInfo(info)
{
}

NonKeyParams::NonKeyParams(const NonKeyParams & other)
    : _storageInfo(other._storageInfo)
{
}

CacheEntryStorageInfo&
NonKeyParams::getStorageInfo()
{
    return _storageInfo;
}

const CacheEntryStorageInfo&
NonKeyParams::getStorageInfo() const
{
    return _storageInfo;
}

void
NonKeyParams::toSerialization(SERIALIZATION_NAMESPACE::SerializationObjectBase* serializationBase)
{
    SERIALIZATION_NAMESPACE::NonKeyParamsSerialization* serialization = dynamic_cast<SERIALIZATION_NAMESPACE::NonKeyParamsSerialization*>(serializationBase);
    if (!serialization) {
        return;
    }
    serialization->dataTypeSize = _storageInfo.dataTypeSize;
    serialization->nComps = _storageInfo.numComponents;
    _storageInfo.bounds.toSerialization(&serialization->bounds);
}

void
NonKeyParams::fromSerialization(const SERIALIZATION_NAMESPACE::SerializationObjectBase& serializationBase)
{
    const SERIALIZATION_NAMESPACE::NonKeyParamsSerialization* serialization = dynamic_cast<const SERIALIZATION_NAMESPACE::NonKeyParamsSerialization*>(&serializationBase);
    if (!serialization) {
        return;
    }
    _storageInfo.dataTypeSize = serialization->dataTypeSize;
    _storageInfo.numComponents = serialization->nComps;
    _storageInfo.mode = eStorageModeDisk;
    _storageInfo.bounds.fromSerialization(serialization->bounds);
}

NATRON_NAMESPACE_EXIT;
