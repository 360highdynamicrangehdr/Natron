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


#ifndef NATRON_SERIALIZATION_KNOBTABLEITEMSERIALIZATION_H
#define NATRON_SERIALIZATION_KNOBTABLEITEMSERIALIZATION_H

#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/SerializationFwd.h"

#define kSerializationRotoGroupTag "Group"
#define kSerializationTrackTag "Track"
#define kSerializationCompLayerTag "Layer"
#define kSerializationOpenedBezierTag "OpenedBezier"
#define kSerializationClosedBezierTag "Bezier"
#define kSerializationOpenedBezierTag "OpenedBezier"
#define kSerializationStrokeBrushTypeSolid "Solid"
#define kSerializationStrokeBrushTypeEraser "Eraser"
#define kSerializationStrokeBrushTypeClone "Clone"
#define kSerializationStrokeBrushTypeReveal "Reveal"
#define kSerializationStrokeBrushTypeBlur "Blur"
#define kSerializationStrokeBrushTypeSharpen "Sharpen"
#define kSerializationStrokeBrushTypeSmear "Smear"
#define kSerializationStrokeBrushTypeDodge "Dodge"
#define kSerializationStrokeBrushTypeBurn "Burn"


SERIALIZATION_NAMESPACE_ENTER;


class KnobTableItemSerialization
: public SerializationObjectBase
{

protected:

    // Used to prevent the encode function to wrap the encoding in a map if a derived
    // class already used a map
    bool _emitMap;
public:

    // Tag type to uniquely identify the item type to YAML
    std::string verbatimTag;

    std::string scriptName;
    std::string label;

    KnobSerializationList knobs;

    std::list<KnobTableItemSerializationPtr> children;


    // For each view, the animation of the user keyframes.
    std::map<std::string, CurveSerialization> animationCurves;

    KnobTableItemSerialization()
    : _emitMap(true)
    , verbatimTag()
    , scriptName()
    , label()
    , knobs()
    , children()
    , animationCurves()
    {

    }

    virtual void encode(YAML::Emitter& em) const OVERRIDE;

    virtual void decode(const YAML::Node& node) OVERRIDE;
};

class KnobItemsTableSerialization
: public SerializationObjectBase
{
public:
    
    // During d&d it is useful to find from which node it is originated
    // This is the fully qualified name of the node, e.g: Group1.Blur1
    std::string nodeScriptName;
    
    // A unique identifier for the table so that one does not attempt to deserialize a table of one type to another one
    std::string tableIdentifier;

    std::list<KnobTableItemSerializationPtr> items;

    
    KnobItemsTableSerialization()
    : nodeScriptName()
    , tableIdentifier()
    , items()
    {
        
    }
    
    virtual void encode(YAML::Emitter& em) const OVERRIDE;
    
    virtual void decode(const YAML::Node& node) OVERRIDE;
    
};

SERIALIZATION_NAMESPACE_EXIT;

#endif // NATRON_SERIALIZATION_KNOBTABLEITEMSERIALIZATION_H
