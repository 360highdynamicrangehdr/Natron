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

#include "Knob.h"
#include "KnobPrivate.h"

#include <sstream> // stringstream
#include <string>

#include "Engine/KnobItemsTable.h"
#include "Engine/Noise.h"
#include "Engine/PyExprUtils.h"
#include "Global/StrUtils.h"

// reduce object size:
// we only include exprtk.hpp here, no need have visible template instanciations since it's not used elsewhere

// include everything included by exprtk.hpp, because we want to put exprtk itself in an anonymous namespace
#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef __GNUC__
//#if defined(__CYGWIN__) || defined(__MINGW32__)
// exprtk requires -Wa,-mbig-obj on mingw, but there is a bug that prevents linking if not using -O3
// see:
// - https://sourceforge.net/p/mingw-w64/discussion/723797/thread/c6b70624/
// - https://github.com/assimp/assimp/issues/177#issuecomment-217605051
// - http://discourse.chaiscript.com/t/compiled-in-std-lib-with-mingw/66/2
#ifndef __clang__
#pragma GCC push_options
#pragma GCC optimize ("-O3")
#endif
//#endif
#pragma GCC visibility push(hidden)
#endif

NATRON_NAMESPACE_ANONYMOUS_ENTER
#include "exprtk.hpp"
NATRON_NAMESPACE_ANONYMOUS_EXIT

#ifdef __GNUC__
#pragma GCC visibility pop
#ifndef __clang__
#pragma GCC pop_options
#endif

#endif

NATRON_NAMESPACE_ENTER


using std::vector;
using std::string;
using std::stringstream;
using std::pair;
using std::make_pair;
using boost::shared_ptr;


NATRON_NAMESPACE_ANONYMOUS_ENTER


typedef double exprtk_scalar_t;
typedef exprtk::symbol_table<exprtk_scalar_t> exprtk_symbol_table_t;
typedef exprtk::type_store<exprtk_scalar_t> exprtk_type_store_t;
typedef exprtk::expression<exprtk_scalar_t> exprtk_expression_t;
typedef exprtk::results_context<exprtk_scalar_t> exprtk_results_context_t;
typedef exprtk::parser<exprtk_scalar_t> exprtk_parser_t;
typedef exprtk::igeneric_function<exprtk_scalar_t> exprtk_igeneric_function_t;
typedef exprtk::ivararg_function<exprtk_scalar_t> exprtk_ivararg_function_t;
typedef exprtk::ifunction<exprtk_scalar_t> exprtk_ifunction_t;
typedef shared_ptr<exprtk_igeneric_function_t> exprtk_igeneric_function_ptr;
typedef shared_ptr<exprtk_ivararg_function_t> exprtk_ivararg_function_ptr;
typedef shared_ptr<exprtk_ifunction_t> exprtk_ifunction_ptr;
typedef vector<pair<string, exprtk_igeneric_function_ptr > > exprtk_igeneric_function_table_t;
typedef vector<pair<string, exprtk_ivararg_function_ptr > > exprtk_ivararg_function_table_t;
typedef vector<pair<string, exprtk_ifunction_ptr > > exprtk_ifunction_table_t;

NATRON_NAMESPACE_ANONYMOUS_EXIT


/**
 * @brief All data that must be kept around for the expression to work.
 * Since the expression is not thread safe, we compile 1 expression for each thread to enable
 * concurrent evaluation of the same expression.
 **/
struct NATRON_NAMESPACE::KnobExprExprTk::ExpressionData
{
    // The exprtk expression object
    shared_ptr<exprtk_expression_t> expressionObject;

    // We hold functions here because the expression object itself does not hold a copy of the functions
    exprtk_ifunction_table_t functions;
    exprtk_ivararg_function_table_t varargFunctions;
    exprtk_igeneric_function_table_t genericFunctions;
};

KnobExprExprTk::ExpressionDataPtr
NATRON_NAMESPACE::KnobExprExprTk::createData()
{
    return KnobExprExprTk::ExpressionDataPtr(new ExpressionData);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

template <typename T, typename FuncType>
void
registerFunction(const string& name,
                 vector<pair<string, shared_ptr<FuncType> > >* functions)
{
    shared_ptr<FuncType> ptr(new T);
    functions->push_back( make_pair(name, ptr) );
}

struct boxstep
    : public exprtk_ifunction_t
{
    boxstep() : exprtk_ifunction_t(2) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& a)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::boxstep(x, a);
    }
};

struct linearstep
    : public exprtk_ifunction_t
{
    linearstep() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& a,
                               const exprtk_scalar_t& b)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::linearstep(x, a, b);
    }
};

struct smoothstep
    : public exprtk_ifunction_t
{
    smoothstep() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& a,
                               const exprtk_scalar_t& b)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::smoothstep(x, a, b);
    }
};

struct gaussstep
    : public exprtk_ifunction_t
{
    gaussstep() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& a,
                               const exprtk_scalar_t& b)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::gaussstep(x, a, b);
    }
};

struct remap
    : public exprtk_ifunction_t
{
    remap() : exprtk_ifunction_t(5) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& source,
                               const exprtk_scalar_t& range,
                               const exprtk_scalar_t& falloff,
                               const exprtk_scalar_t& interp)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::remap(x, source, range, falloff, interp);
    }
};

struct mix
    : public exprtk_ifunction_t
{
    mix() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y,
                               const exprtk_scalar_t& alpha)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::mix(x, y, alpha);
    }
};

struct hash
    : public exprtk_ivararg_function_t
{
    exprtk_scalar_t operator()(const vector<double>& arglist)
    {
        return NATRON_PYTHON_NAMESPACE::ExprUtils::hash(arglist);
    }
};

struct noise1
    : public exprtk_ifunction_t
{
    noise1() : exprtk_ifunction_t(1) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x)
    {
        double ret = 0.;
        double input = x;

        // coverity[callee_ptr_arith]
        Noise<1, 1>(&input, &ret);

        return ret;
    }
};

struct noise2
    : public exprtk_ifunction_t
{
    noise2() : exprtk_ifunction_t(2) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y)
    {
        double ret = 0.;
        double input[2] = {x, y};

        // coverity[callee_ptr_arith]
        Noise<2, 1>(input, &ret);

        return ret;
    }
};

struct noise3
    : public exprtk_ifunction_t
{
    noise3() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y,
                               const exprtk_scalar_t& z)
    {
        double ret = 0.;
        double input[3] = {x, y, z};

        // coverity[callee_ptr_arith]
        Noise<3, 1>(input, &ret);

        return ret;
    }
};

struct noise4
    : public exprtk_ifunction_t
{
    noise4() : exprtk_ifunction_t(4) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y,
                               const exprtk_scalar_t& z,
                               const exprtk_scalar_t& w)
    {
        double ret = 0.;
        double input[4] = {x, y, z, w};

        // coverity[callee_ptr_arith]
        Noise<4, 1>(input, &ret);

        return ret;
    }
};

struct turbulence
    : public exprtk_igeneric_function_t
{
    typedef typename exprtk_igeneric_function_t::parameter_list_t parameter_list_t;

    turbulence()
        : exprtk_igeneric_function_t("TTT|TTTT|TTTTT|TTTTTT")
    {}

    virtual exprtk_scalar_t operator()(const std::size_t& overloadIdx,
                                       parameter_list_t parameters) OVERRIDE FINAL
    {
        Q_UNUSED(overloadIdx);
        typedef typename exprtk_igeneric_function_t::generic_type generic_type;
        typedef typename generic_type::scalar_view scalar_t;

        assert(parameters.size() == overloadIdx + 3);
        assert(parameters.size() == 3 || parameters.size() == 4 || parameters.size() == 5 || parameters.size() == 6);
        assert(parameters[0].type == generic_type::e_scalar);
        assert(parameters[1].type == generic_type::e_scalar);
        assert(parameters[2].type == generic_type::e_scalar);
        NATRON_PYTHON_NAMESPACE::Double3DTuple p;
        p.x = scalar_t(parameters[0])();
        p.y = (double)scalar_t(parameters[1])();
        p.z = (double)scalar_t(parameters[2])();


        double octaves = 6;
        double lacunarity = 2.;
        double gain = 0.5;
        if (parameters.size() > 3) {
            octaves = scalar_t(parameters[3])();
        }
        if (parameters.size() > 4) {
            lacunarity = scalar_t(parameters[4])();
        }
        if (parameters.size() > 5) {
            gain = scalar_t(parameters[5])();
        }

        return NATRON_PYTHON_NAMESPACE::ExprUtils::turbulence(p, octaves, lacunarity, gain);
    }
};

struct fbm
    : public exprtk_igeneric_function_t
{
    typedef typename exprtk_igeneric_function_t::parameter_list_t parameter_list_t;

    fbm()
        : exprtk_igeneric_function_t("TTT|TTTT|TTTTT|TTTTTT")
    {}

    virtual exprtk_scalar_t operator()(const std::size_t& overloadIdx,
                                       parameter_list_t parameters) OVERRIDE FINAL
    {
        Q_UNUSED(overloadIdx);
        typedef typename exprtk_igeneric_function_t::generic_type generic_type;
        typedef typename generic_type::scalar_view scalar_t;

        assert(parameters.size() == overloadIdx + 3);
        assert(parameters.size() == 3 || parameters.size() == 4 || parameters.size() == 5 || parameters.size() == 6);
        assert(parameters[0].type == generic_type::e_scalar);
        assert(parameters[1].type == generic_type::e_scalar);
        assert(parameters[2].type == generic_type::e_scalar);
        NATRON_PYTHON_NAMESPACE::Double3DTuple p;
        p.x = scalar_t(parameters[0])();
        p.y = (double)scalar_t(parameters[1])();
        p.z = (double)scalar_t(parameters[2])();

        double octaves = 6;
        double lacunarity = 2.;
        double gain = 0.5;
        if (parameters.size() > 3) {
            octaves = scalar_t(parameters[3])();
        }
        if (parameters.size() > 4) {
            lacunarity = scalar_t(parameters[4])();
        }
        if (parameters.size() > 5) {
            gain = scalar_t(parameters[5])();
        }

        return NATRON_PYTHON_NAMESPACE::ExprUtils::fbm(p, octaves, lacunarity, gain);
    }
};

struct cellnoise
    : public exprtk_ifunction_t
{
    cellnoise() : exprtk_ifunction_t(3) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y,
                               const exprtk_scalar_t& z)
    {
        double result = 0.;
        double input[3] = {x, y, z};

        // coverity[callee_ptr_arith]
        CellNoise<3, 1>(input, &result);

        return result;
    }
};

struct pnoise
    : public exprtk_ifunction_t
{
    pnoise() : exprtk_ifunction_t(6) {}

    exprtk_scalar_t operator()(const exprtk_scalar_t& x,
                               const exprtk_scalar_t& y,
                               const exprtk_scalar_t& z,
                               const exprtk_scalar_t& px,
                               const exprtk_scalar_t& py,
                               const exprtk_scalar_t& pz)
    {
        double result = 0.;
        double p[3] = {x, y, z};
        int pargs[3] = {
            std::max(1, (int)px),
            std::max(1, (int)py),
            std::max(1, (int)pz)
        };

        // coverity[callee_ptr_arith]
        PNoise<3, 1>( (const double*)p, pargs, &result );

        return result;
    }
};

struct random
    : public exprtk_ifunction_t
{
    U32 lastRandomHash;

    random(TimeValue time)
        : exprtk_ifunction_t(2)
        , lastRandomHash(0)
    {
        // Make the hash vary from time
        {
            alias_cast_float ac;
            ac.data = (float)time;
            lastRandomHash += ac.raw;
        }
    }

    exprtk_scalar_t operator()(const exprtk_scalar_t& min,
                               const exprtk_scalar_t& max)
    {
        lastRandomHash = hashFunction(lastRandomHash);

        return ( (double)lastRandomHash / (double)0x100000000LL ) * (max - min)  + min;
    }
};

struct randomInt
    : public exprtk_ifunction_t
{
    U32 lastRandomHash;

    randomInt(TimeValue time)
        : exprtk_ifunction_t(2)
        , lastRandomHash(0)
    {
        // Make the hash vary from time
        {
            alias_cast_float ac;
            ac.data = (float)time;
            lastRandomHash += ac.raw;
        }
    }

    exprtk_scalar_t operator()(const exprtk_scalar_t& min,
                               const exprtk_scalar_t& max)
    {
        lastRandomHash = hashFunction(lastRandomHash);

        return int( ( (double)lastRandomHash / (double)0x100000000LL ) * ( (int)max - (int)min )  + (int)min );
    }
};

struct numtostr
    : public exprtk_igeneric_function_t
{
    typedef typename exprtk_igeneric_function_t::parameter_list_t parameter_list_t;

    numtostr()
        : exprtk_igeneric_function_t("T|TS|TST", exprtk_igeneric_function_t::e_rtrn_string)
    {
        /*overloads:
           str(value)
           str(value, format)
           str(value, format, precision)
         */
    }

    virtual exprtk_scalar_t operator()(const std::size_t& overloadIdx,
                                       string& result,
                                       parameter_list_t parameters) OVERRIDE FINAL
    {
        Q_UNUSED(overloadIdx);
        typedef typename exprtk_igeneric_function_t::generic_type generic_type;
        typedef typename generic_type::scalar_view scalar_t;
        typedef typename generic_type::string_view string_t;

        assert(parameters.size() == overloadIdx + 1);
        assert(parameters.size() == 1 || parameters.size() == 2 || parameters.size() == 3);

        double value = scalar_t(parameters[0])();
        string format;
        format += 'f';
        if (parameters.size() > 1) {
            format = exprtk::to_str( string_t(parameters[1]) );
            if ( (format != "f") && (format != "g") && (format != "e") && (format != "G") && (format != "E") ) {
                return 0.;
            }
        }
        int precision = 6;
        if (parameters.size() > 2) {
            precision = std::floor( scalar_t(parameters[2])() );
        }

        QString str = QString::number(value, format[0], precision);
        result = str.toStdString();

        return 1.;
    }
};

void
addVarargFunctions(TimeValue /*time*/,
                   exprtk_ivararg_function_table_t* functions)
{
    registerFunction<hash>("hash", functions);
}

void
addFunctions(TimeValue time,
             exprtk_ifunction_table_t* functions)
{
    registerFunction<boxstep>("boxstep", functions);
    registerFunction<linearstep>("linearstep", functions);
    registerFunction<smoothstep>("smoothstep", functions);
    registerFunction<gaussstep>("gaussstep", functions);
    registerFunction<remap>("remap", functions);
    registerFunction<mix>("mix", functions);
    registerFunction<noise1>("noise1", functions);
    registerFunction<noise2>("noise2", functions);
    registerFunction<noise3>("noise3", functions);
    registerFunction<noise4>("noise4", functions);
    registerFunction<pnoise>("pnoise", functions);
    registerFunction<cellnoise>("cellnoise", functions);
    {
        shared_ptr<exprtk_ifunction_t> ptr( new random(time) );
        functions->push_back( make_pair("random", ptr) );
    }
    {
        shared_ptr<exprtk_ifunction_t> ptr( new randomInt(time) );
        functions->push_back( make_pair("randomInt", ptr) );
    }
}

void
addGenericFunctions(TimeValue /*time*/,
                    exprtk_igeneric_function_table_t* functions)
{
    registerFunction<turbulence>("turbulence", functions);
    registerFunction<fbm>("fbm", functions);
    registerFunction<numtostr>("str", functions);
}

// Some functions (random) hold an internal state. Instead of using the same state for all threads,
// We create a copy of the function update in this symbol table
void
makeLocalCopyOfStateFunctions(TimeValue time,
                              exprtk_symbol_table_t& symbol_table,
                              exprtk_ifunction_table_t* functions)
{
    symbol_table.remove_function("random");
    symbol_table.remove_function("randomInt");
    {
        shared_ptr<exprtk_ifunction_t> ptr( new random(time) );
        functions->push_back( make_pair("random", ptr) );
    }
    {
        shared_ptr<exprtk_ifunction_t> ptr( new randomInt(time) );
        functions->push_back( make_pair("randomInt", ptr) );
    }
}

bool
isDimensionIndex(const string& str,
                 int* index)
{
    if ( (str == "r") || (str == "x") || (str == "0") ) {
        *index = 0;

        return true;
    }
    if ( (str == "g") || (str == "y") || (str == "1") ) {
        *index = 1;

        return true;
    }
    if ( (str == "b") || (str == "z") || (str == "2") ) {
        *index = 2;

        return true;
    }
    if ( (str == "a") || (str == "w") || (str == "3") ) {
        *index = 3;

        return true;
    }

    return false;
}

class SymbolResolver
{
    KnobI* _knob;
    DimIdx _dimension;
    ViewIdx _view;
    string _symbol;

public:

    enum ResultTypeEnum
    {
        eResultTypeInvalid,
        eResultTypeKnobValue,
        eResultTypeKnobChoiceOption,
        eResultTypeEffectRoD,
        eResultTypeObjectName,
    };

    ResultTypeEnum _resultType;
    string _objectName;
    string _error;
    bool _testingEnabled;

    // If result is eResultTypeEffectRoD, this is the effect on which to retrieve the property
    EffectInstancePtr _effectProperty;

    // If the result is eResultTypeKnobValue, this is the knob on which to retrieve the value
    KnobIPtr _targetKnob;
    ViewIdx _targetView;
    DimIdx _targetDimension;

    SymbolResolver(KnobI* knob,
                   DimIdx dimension,
                   ViewIdx view,
                   const string& symbol)
        : _knob(knob)
        , _dimension(dimension)
        , _view(view)
        , _symbol(symbol)
        , _resultType(eResultTypeInvalid)
        , _error()
        , _testingEnabled(false)
    {
        resolve();
    }

private:

    void resolve()
    {
        // Split the variable with dots
        vector<string> splits = StrUtils::split(_symbol, '.');

        EffectInstancePtr currentNode = getThisNode();
        KnobHolderPtr currentHolder = _knob->getHolder();
        KnobTableItemPtr currentTableItem = getThisTableItem();
        NodeCollectionPtr currentGroup = getThisGroup();
        KnobIPtr currentKnob;
        DimIdx currentDimension = _dimension;
        ViewIdx currentView = _view;
        assert(currentNode && currentGroup);

        // If "exists" is suffixed, we never fail but instead return 0 or 1
        _testingEnabled = !splits.empty() && splits.back() == "exists";
        if (_testingEnabled) {
            splits.resize(splits.size() - 1);
        }
        for (std::size_t i = 0; i < splits.size(); ++i) {
            bool isLastToken = (i == splits.size() - 1);
            const string& token = splits[i];


            {
                NodeCollectionPtr curGroupOut;
                if ( checkForGroup(token, &curGroupOut) ) {
                    // If we caught a group, check if it is a node too
                    currentGroup = curGroupOut;
                    currentNode = toNodeGroup(currentGroup);
                    currentHolder = currentNode;
                    currentTableItem.reset();
                    currentKnob.reset();
                    if (isLastToken) {
                        if (_testingEnabled) {
                            // If testing is enabled, set the result to be valid
                            _resultType = eResultTypeKnobValue;
                        } else {
                            _error = _symbol + ": a variable can only be bound to a value";
                        }

                        return;
                    }
                    continue;
                }
            }

            {
                EffectInstancePtr curNodeOut;
                if ( checkForNode(token, currentGroup, currentNode, &curNodeOut) ) {
                    // If we caught a node, check if it is a group too
                    currentNode = curNodeOut;
                    currentGroup = toNodeGroup(currentNode);
                    currentHolder = currentNode;
                    currentTableItem.reset();
                    currentKnob.reset();
                    if (isLastToken) {
                        if (_testingEnabled) {
                            // If testing is enabled, set the result to be valid
                            _resultType = eResultTypeKnobValue;
                        } else {
                            _error = _symbol + ": a variable can only be bound to a value";
                        }

                        return;
                    }
                    continue;
                }
            }

            {
                KnobTableItemPtr curTableItemOut;
                if ( checkForTableItem(token, currentHolder, &curTableItemOut) ) {
                    // If we caught a node, check if it is a group too
                    currentTableItem = curTableItemOut;
                    currentHolder = currentTableItem;
                    currentNode.reset();
                    currentGroup.reset();
                    currentKnob.reset();
                    if (isLastToken) {
                        if (_testingEnabled) {
                            // If testing is enabled, set the result to be valid
                            _resultType = eResultTypeKnobValue;
                        } else {
                            _error = _symbol + ": a variable can only be bound to a value";
                        }

                        return;
                    }
                    continue;
                }
            }

            {
                if ( checkForProject(token) ) {
                    ProjectPtr proj = _knob->getHolder()->getApp()->getProject();
                    currentHolder = proj;
                    currentTableItem.reset();
                    currentNode.reset();
                    currentGroup = proj;
                    currentKnob.reset();
                    if (isLastToken) {
                        if (_testingEnabled) {
                            // If testing is enabled, set the result to be valid
                            _resultType = eResultTypeKnobValue;
                        } else {
                            _error = _symbol + ": a variable can only be bound to a value";
                        }

                        return;
                    }
                    continue;
                }
            }

            {
                KnobIPtr curKnobOut;
                if ( checkForKnob(token, currentHolder, &curKnobOut) ) {
                    // If we caught a node, check if it is a group too
                    currentKnob = curKnobOut;
                    currentHolder.reset();
                    currentTableItem.reset();
                    currentNode.reset();
                    currentGroup.reset();
                    if (isLastToken) {
                        if (currentKnob->getNDimensions() > 1) {
                            if (_testingEnabled) {
                                // If testing is enabled, set the result to be valid
                                _resultType = eResultTypeKnobValue;
                            } else {
                                _error = _symbol + ": this parameter has multiple dimension, please specify one";
                            }

                            return;
                        } else {
                            // single dimension, return the value of the knob at dimension 0
                            _targetView = _view;
                            _targetKnob = currentKnob;
                            _targetDimension = DimIdx(0);
                            _resultType = eResultTypeKnobValue;

                            return;
                        }
                    }
                    continue;
                }
            }


            if ( currentKnob && checkForDimension(token, currentKnob, &currentDimension) ) {
                // If we caught a node, check if it is a group too
                currentHolder.reset();
                currentTableItem.reset();
                currentNode.reset();
                currentGroup.reset();
                if (!isLastToken) {
                    _error = _symbol + ": a variable can only be bound to a value";

                    return;
                }
                _targetKnob = currentKnob;
                _targetDimension = currentDimension;
                _targetView = currentView;
                _resultType = eResultTypeKnobValue;

                return;
            }

            if ( currentKnob && (token == "option") && dynamic_cast<KnobChoice*>( currentKnob.get() ) ) {
                // For a KnobChoice, we can get the option string directly
                if (!isLastToken) {
                    _error = _symbol + ": a variable can only be bound to a value";

                    return;
                }
                _targetKnob = currentKnob;
                _targetDimension = currentDimension;
                _targetView = currentView;
                _resultType = eResultTypeKnobChoiceOption;

                return;
            }

            // Check is the user wants the region of definition of an effect
            if (currentNode) {
                if (token == "rod") {
                    // The rod of the effect
                    _resultType = eResultTypeEffectRoD;
                    _effectProperty = currentNode;

                    return;
                }
            }

            // Now check if we want the name of an object

            if (token == "name") {
                bool gotName = false;
                if (currentKnob) {
                    gotName = true;
                    _objectName = currentKnob->getName();
                } else if (currentTableItem) {
                    gotName = true;
                    _objectName = currentTableItem->getScriptName_mt_safe();
                } else if (currentNode) {
                    gotName = true;
                    _objectName = currentNode->getScriptName_mt_safe();
                }

                if (gotName) {
                    _resultType = eResultTypeObjectName;

                    return;
                }
            }

            _error = "Undefined symbol " + _symbol;
            _resultType = eResultTypeInvalid;

            return;
        } // for each splits
    } // resolve

    NodeCollectionPtr getThisGroup() const
    {
        EffectInstancePtr thisNode = getThisNode();

        if (!thisNode) {
            return NodeCollectionPtr();
        }

        return thisNode->getNode()->getGroup();
    }

    bool checkForGroup(const string& str,
                       NodeCollectionPtr* retIsGroup) const
    {
        retIsGroup->reset();
        if (str == "thisGroup") {
            *retIsGroup = getThisGroup();

            return true;
        }

        return false;
    }

    EffectInstancePtr getThisNode() const
    {
        KnobHolderPtr holder = _knob->getHolder();

        if (!holder) {
            return EffectInstancePtr();
        }
        EffectInstancePtr effect = toEffectInstance( _knob->getHolder() );
        KnobTableItemPtr tableItem = toKnobTableItem( _knob->getHolder() );
        if (tableItem) {
            effect = tableItem->getModel()->getNode()->getEffectInstance();
        }

        return effect;
    }

    bool checkForNode(const string& str,
                      const NodeCollectionPtr& callerGroup,
                      const EffectInstancePtr& callerIsNode,
                      EffectInstancePtr* retIsNode) const
    {
        retIsNode->reset();
        if (str == "thisNode") {
            *retIsNode = getThisNode();

            return true;
        } else if (callerGroup) {
            // Loop over all nodes within the caller group
            NodePtr foundNode = callerGroup->getNodeByName(str);
            if (foundNode) {
                *retIsNode = foundNode->getEffectInstance();

                return true;
            }
        }

        // Check for an inputNumber
        if (callerIsNode) {
            string prefix("input");
            std::size_t foundPrefix = str.find(prefix);
            if (foundPrefix != string::npos) {
                string inputNumberStr = str.substr( prefix.size() );
                int inputNb = -1;
                if ( inputNumberStr.empty() ) {
                    inputNb = 0;
                } else {
                    bool isValidNumber = true;
                    for (std::size_t i = 0; i < inputNumberStr.size(); ++i) {
                        if ( !std::isdigit(inputNumberStr[i]) ) {
                            isValidNumber = false;
                            break;
                        }
                    }
                    if (isValidNumber) {
                        inputNb = std::atoi( inputNumberStr.c_str() );
                    }
                }
                if (inputNb != -1) {
                    *retIsNode = callerIsNode->getInputMainInstance(inputNb);
                    if (*retIsNode) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    KnobTableItemPtr getThisTableItem() const
    {
        KnobHolderPtr holder = _knob->getHolder();

        if (!holder) {
            return KnobTableItemPtr();
        }

        return toKnobTableItem( _knob->getHolder() );
    }

    bool checkForProject(const string& str) const
    {
        if (str == "app") {
            return true;
        }

        return false;
    }

    bool checkForTableItem(const string& str,
                           const KnobHolderPtr& callerHolder,
                           KnobTableItemPtr* retIsTableItem) const
    {
        retIsTableItem->reset();
        if (str == "thisItem") {
            *retIsTableItem = getThisTableItem();

            return true;
        } else if (callerHolder) {
            EffectInstancePtr callerIsEffect = toEffectInstance(callerHolder);
            KnobTableItemPtr callerIsTableItem = toKnobTableItem(callerHolder);
            assert(callerIsEffect || callerIsTableItem);
            if (callerIsEffect) {
                KnobItemsTablePtr table = callerIsEffect->getItemsTable();
                if (table) {
                    *retIsTableItem = table->getTopLevelItemByScriptName(str);
                    if (*retIsTableItem) {
                        return true;
                    }
                }
            } else if (callerIsTableItem) {
                *retIsTableItem = callerIsTableItem->getChildItemByScriptName(str);
                if (*retIsTableItem) {
                    return true;
                }
            }
        }

        return false;
    }

    KnobIPtr getThisKnob() const
    {
        return _knob->shared_from_this();
    }

    bool checkForKnob(const string& str,
                      const KnobHolderPtr& callerHolder,
                      KnobIPtr* retIsKnob) const
    {
        retIsKnob->reset();
        if (str == "thisKnob") {
            *retIsKnob = getThisKnob();

            return true;
        } else if (callerHolder) {
            *retIsKnob = callerHolder->getKnobByName(str);
            if (*retIsKnob) {
                return true;
            }
        }

        return false;
    }

    bool checkForDimension(const string& str,
                           const KnobIPtr& callerKnob,
                           DimIdx* retIsDimension) const
    {
        if (str == "dimension") {
            *retIsDimension = _dimension;

            return true;
        } else if (callerKnob) {
            int idx;
            if ( isDimensionIndex(str, &idx) ) {
                *retIsDimension = DimIdx(idx);

                return true;
            }
        }

        return false;
    }
};


struct UnknownSymbolResolver
    : public exprtk_parser_t::unknown_symbol_resolver
{
    KnobHelper* _knob;
    TimeValue _time;
    DimIdx _dimension;
    ViewIdx _view;
    KnobExprExprTk* _ret;
    usr_variable_user_type _varType;
    exprtk_scalar_t _resolvedScalar;
    vector<exprtk_scalar_t> _resolvedVector;
    string _resolvedString;


    UnknownSymbolResolver(KnobHelper* knob,
                          TimeValue time,
                          DimIdx dimension,
                          ViewIdx view,
                          KnobExprExprTk* ret)
        : exprtk_parser_t::unknown_symbol_resolver()
        , _knob(knob)
        , _time(time)
        , _dimension(dimension)
        , _view(view)
        , _ret(ret)
        , _varType(e_usr_variable_user_type_scalar)
        , _resolvedScalar(0)
        , _resolvedVector()
        , _resolvedString()
    {
    }

    virtual usr_symbol_type getSymbolType() const OVERRIDE FINAL
    {
        // Values are variables since we will update them later on
        return e_usr_variable_type;
    }

    virtual usr_variable_user_type getVariableType() const OVERRIDE FINAL
    {
        return _varType;
    }

    virtual exprtk_scalar_t getResolvedScalar() OVERRIDE FINAL
    {
        assert(_varType == e_usr_variable_user_type_scalar);

        return _resolvedScalar;
    }

    virtual string& getResolvedString() OVERRIDE FINAL
    {
        assert(_varType == e_usr_variable_user_type_string);

        return _resolvedString;
    }

    virtual vector<exprtk_scalar_t>& getResolvedVector()  OVERRIDE FINAL
    {
        assert(_varType == e_usr_variable_user_type_vector);

        return _resolvedVector;
    }

    virtual bool process(const string& unknown_symbol,
                         string& error_message) OVERRIDE FINAL
    {
        SymbolResolver resolver(_knob, _dimension, _view, unknown_symbol);

        if (resolver._testingEnabled) {
            _varType = e_usr_variable_user_type_scalar;
            if (resolver._resultType == SymbolResolver::eResultTypeInvalid) {
                _resolvedScalar = 0;
            } else {
                _resolvedScalar = 1;
            }

            return true;
        }
        switch (resolver._resultType) {
        case SymbolResolver::eResultTypeInvalid: {
            error_message = "Error when parsing symbol " + unknown_symbol;
            if ( !resolver._error.empty() ) {
                error_message += ": " + resolver._error;
            }
        }

            return false;
        case SymbolResolver::eResultTypeObjectName: {
            _resolvedString = resolver._objectName;
            _varType = e_usr_variable_user_type_string;
            break;
        }
        case SymbolResolver::eResultTypeKnobChoiceOption: {
            KnobChoice* isChoice = dynamic_cast<KnobChoice*>(_knob);
            if (!isChoice) {
                return false;
            }
            _resolvedString = isChoice->getActiveEntry(_view).id;
            _varType = e_usr_variable_user_type_string;
            break;
        }
        case SymbolResolver::eResultTypeEffectRoD: {
            GetRegionOfDefinitionResultsPtr results;
            ActionRetCodeEnum stat = resolver._effectProperty->getRegionOfDefinition_public(_time, RenderScale(1.), _view, &results);
            _varType = e_usr_variable_user_type_vector;
            _resolvedVector.resize(4);
            if ( isFailureRetCode(stat) ) {
                _resolvedVector[0] = _resolvedVector[1] = _resolvedVector[2] = _resolvedVector[3] = 0.;
            } else {
                const RectD& rod = results->getRoD();
                _resolvedVector[0] = rod.x1;
                _resolvedVector[1] = rod.y1;
                _resolvedVector[2] = rod.x2;
                _resolvedVector[3] = rod.y2;
            }
            break;
        }

        case SymbolResolver::eResultTypeKnobValue: {
            // Register the target knob as a dependency of this expression
            {
                KnobDimViewKey dep;
                dep.knob = resolver._targetKnob;
                dep.dimension = resolver._targetDimension;
                dep.view = resolver._targetView;
                _ret->knobDependencies.insert( make_pair(unknown_symbol, dep) );
            }


            // Return the value of the knob at the given dimension
            KnobBoolBasePtr isBoolean = toKnobBoolBase(resolver._targetKnob);
            KnobStringBasePtr isString = toKnobStringBase(resolver._targetKnob);
            KnobIntBasePtr isInt = toKnobIntBase(resolver._targetKnob);
            KnobDoubleBasePtr isDouble = toKnobDoubleBase(resolver._targetKnob);
            if (isBoolean) {
                _resolvedScalar = exprtk_scalar_t( isBoolean->getValueAtTime(_time, resolver._targetDimension, resolver._targetView) );
                _varType = e_usr_variable_user_type_scalar;
            } else if (isInt) {
                _resolvedScalar = exprtk_scalar_t( isInt->getValueAtTime(_time, resolver._targetDimension, resolver._targetView) );
                _varType = e_usr_variable_user_type_scalar;
            } else if (isDouble) {
                _resolvedScalar = exprtk_scalar_t( isDouble->getValueAtTime(_time, resolver._targetDimension, resolver._targetView) );
                _varType = e_usr_variable_user_type_scalar;
            } else if (isString) {
                _varType = e_usr_variable_user_type_string;
                _resolvedString = isString->getValueAtTime(_time, resolver._targetDimension, resolver._targetView);
            }
            break;
        }
        } // switch

        return true;
    } // process
};

struct curve_func
    : public exprtk_igeneric_function_t
{
    typedef typename exprtk_igeneric_function_t::parameter_list_t parameter_list_t;
    KnobIWPtr _knob;
    ViewIdx _view;

    curve_func(const KnobIPtr& knob,
               ViewIdx view)
        : exprtk_igeneric_function_t("T|TT|TTS")
        , _knob(knob)
        , _view(view)
    {
        /*
           Overloads:
           curve(frame)
           curve(frame, dimension)
           curve(frame, dimension, view)
         */
    }

    virtual exprtk_scalar_t operator()(const std::size_t& overloadIdx,
                                       parameter_list_t parameters) OVERRIDE FINAL
    {
        typedef typename exprtk_igeneric_function_t::generic_type generic_type;
        typedef typename generic_type::scalar_view scalar_t;
        typedef typename generic_type::string_view string_t;

        assert( overloadIdx + 1 == parameters.size() );
        assert(parameters.size() == 1 || parameters.size() == 2 || parameters.size() == 3);
        assert(parameters[0].type == generic_type::e_scalar);
        assert(parameters[1].type == generic_type::e_scalar);

        KnobIPtr knob = _knob.lock();
        if (!knob) {
            return 0.;
        }

        TimeValue frame(0);
        ViewIdx view(0);
        DimIdx dimension(0);
        frame = TimeValue( scalar_t(parameters[0])() );
        if (parameters.size() > 1) {
            dimension = DimIdx( (int)scalar_t(parameters[1])() );
        }
        if (parameters.size() > 2) {
            string viewStr = exprtk::to_str( string_t(parameters[2]) );
            if (viewStr == "view") {
                // use the current view
                view = _view;
            } else {
                // Find the view by name in the project
                const vector<string>& views = knob->getHolder()->getApp()->getProject()->getProjectViewNames();
                for (std::size_t i = 0; i < views.size(); ++i) {
                    if (views[i] == viewStr) {
                        view = ViewIdx(i);
                    }
                }
            }
        }

        return knob->getRawCurveValueAt(frame, view, dimension);
    }
};

void
addStandardFunctions(const string& expr,
                     TimeValue time,
                     exprtk_symbol_table_t& symbol_table,
                     exprtk_ifunction_table_t& functions,
                     exprtk_ivararg_function_table_t& varargFunctions,
                     exprtk_igeneric_function_table_t& genericFunctions,
                     string* modifiedExpression)
{
    // Add all functions from the API to the symbol table

    addFunctions(time, &functions);
    addGenericFunctions(time, &genericFunctions);
    addVarargFunctions(time, &varargFunctions);

    for (std::size_t i = 0; i < functions.size(); ++i) {
        bool ok = symbol_table.add_function(functions[i].first, *functions[i].second);
        assert(ok);
        Q_UNUSED(ok);
    }
    for (std::size_t i = 0; i < varargFunctions.size(); ++i) {
        bool ok = symbol_table.add_function(varargFunctions[i].first, *varargFunctions[i].second);
        assert(ok);
        Q_UNUSED(ok);
    }

    for (std::size_t i = 0; i < genericFunctions.size(); ++i) {
        bool ok = symbol_table.add_function(genericFunctions[i].first, *genericFunctions[i].second);
        assert(ok);
        Q_UNUSED(ok);
    }

    if (modifiedExpression) {
        // Modify the expression so that the last line is modified by "var exprResult:= ..."; return [exprResult]"
        *modifiedExpression = expr;
        {
            // Check for the last ';' indicating the previous statement
            std::size_t foundLastStatement = modifiedExpression->find_last_of(';');
            string toPrepend = "var NatronExprtkExpressionResult := ";
            bool mustAddSemiColon = true;
            if (foundLastStatement == string::npos) {
                // There's no ';', the expression is a single statement, pre-pend directly
                modifiedExpression->insert(0, toPrepend);
            } else {
                // Check that there's no whitespace after the last ';' otherwise the user
                // just wrote a ';' after the last statement, which is allowed.
                bool hasNonWhitespace = false;
                for (std::size_t i = foundLastStatement + 1; i < modifiedExpression->size(); ++i) {
                    if ( !std::isspace( (*modifiedExpression)[i] ) ) {
                        hasNonWhitespace = true;
                        break;
                    }
                }
                if (!hasNonWhitespace) {
                    foundLastStatement = modifiedExpression->find_last_of(';', foundLastStatement - 1);
                    mustAddSemiColon = false;
                }
                if (foundLastStatement == string::npos) {
                    modifiedExpression->insert(0, toPrepend);
                } else {
                    modifiedExpression->insert(foundLastStatement + 1, toPrepend);
                }
            }
            if (mustAddSemiColon) {
                (*modifiedExpression) += ';';
            }
            string toAppend = "\nreturn [NatronExprtkExpressionResult]";
            modifiedExpression->append(toAppend);
        }
    } // modifiedExpression
} // addStandardFunctions

bool
parseExprtkExpression(const string& originalExpression,
                      const string& expressionString,
                      exprtk_parser_t& parser,
                      exprtk_expression_t& expressionObject,
                      string* error)
{
    // Compile the expression
    if ( !parser.compile(expressionString, expressionObject) ) {
        stringstream ss;
        ss << "Error(s) while compiling the following expression:" << std::endl;
        ss << originalExpression << std::endl;
        for (std::size_t i = 0; i < parser.error_count(); ++i) {
            // Include the specific nature of each error
            // and its position in the expression string.

            exprtk::parser_error::type error = parser.get_error(i);
            ss << "Error: " << i << " Position: " << error.token.position << std::endl;
            ss << "Type: " << exprtk::parser_error::to_str(error.mode) << std::endl;
            ss << "Message: " << error.diagnostic << std::endl;
        }
        *error = ss.str();

        return false;
    }

    return true;
} // parseExprtkExpression

KnobHelper::ExpressionReturnValueTypeEnum
handleExprTkReturn(exprtk_expression_t& expressionObject,
                   double* retValueIsScalar,
                   string* retValueIsString,
                   string* error)
{
    const exprtk_results_context_t& results = expressionObject.results();

    if (results.count() != 1) {
        *error = "The expression must return one value using the \"return\" keyword";

        return KnobHelper::eExpressionReturnValueTypeError;
    }

    switch (results[0].type) {
    case exprtk_type_store_t::e_scalar:
        *retValueIsScalar = (double)*reinterpret_cast<exprtk_scalar_t*>(results[0].data);

        return KnobHelper::eExpressionReturnValueTypeScalar;
    case exprtk_type_store_t::e_string:
        *retValueIsString = string( reinterpret_cast<const char*>(results[0].data) );

        return KnobHelper::eExpressionReturnValueTypeString;

    case exprtk_type_store_t::e_vector:
    case exprtk_type_store_t::e_unknown:
        *error = "The expression must either return a scalar or string value depending on the parameter type";

        return KnobHelper::eExpressionReturnValueTypeError;
    }
} // handleExprTkReturn

NATRON_NAMESPACE_ANONYMOUS_EXIT


void
KnobHelperPrivate::validateExprTkExpression(const string& expression,
                                            DimIdx dimension,
                                            ViewIdx view,
                                            string* resultAsString,
                                            KnobExprExprTk* ret) const
{
    // Symbol table containing all variables that the user may use but that we do not pre-declare, such
    // as knob values etc...
    exprtk_symbol_table_t unknown_var_symbol_table;

    // Symbol table containing all pre-declared variables (frame, view etc...)
    exprtk_symbol_table_t symbol_table;
    QThread* curThread = QThread::currentThread();

    KnobExprExprTk::ExpressionDataPtr& data = ret->data[curThread];
    if (!data) {
        data = KnobExprExprTk::createData();
    }
    data->expressionObject.reset(new exprtk_expression_t);
    data->expressionObject->register_symbol_table(unknown_var_symbol_table);
    data->expressionObject->register_symbol_table(symbol_table);

    // Pre-declare the variables with a stub value, they will be updated at evaluation time
    TimeValue time = publicInterface->getCurrentRenderTime();
    KnobIPtr thisShared = publicInterface->shared_from_this();

    {
        double time_f = (double)time;
        symbol_table.add_variable("frame", time_f);
    }
    string viewName = publicInterface->getHolder()->getApp()->getProject()->getViewName(view);
    symbol_table.add_stringvar("view", viewName, false);

    {
        // The object that resolves undefined knob dependencies at compile time
        UnknownSymbolResolver musr(publicInterface, time, dimension, view, ret);
        exprtk_parser_t parser;
        parser.enable_unknown_symbol_resolver(&musr);


        addStandardFunctions(expression, time, symbol_table, data->functions, data->varargFunctions, data->genericFunctions, &ret->modifiedExpression);


        exprtk_igeneric_function_ptr curveFunc( new curve_func(thisShared, view) );
        data->genericFunctions.push_back( make_pair("curve", curveFunc) );
        symbol_table.add_function("curve", *curveFunc);

        string error;
        if ( !parseExprtkExpression(expression, ret->modifiedExpression, parser, *data->expressionObject, &error) ) {
            throw std::runtime_error(error);
        }
    } // parser

    data->expressionObject->value();
    double retValueIsScalar;
    string error;
    KnobHelper::ExpressionReturnValueTypeEnum stat = handleExprTkReturn(*data->expressionObject, &retValueIsScalar, resultAsString, &error);
    switch (stat) {
    case KnobHelper::eExpressionReturnValueTypeError:
        throw std::runtime_error(error);
        break;
    case KnobHelper::eExpressionReturnValueTypeScalar: {
        stringstream ss;
        ss << retValueIsScalar;
        *resultAsString = ss.str();
        break;
    }
    case KnobHelper::eExpressionReturnValueTypeString:
        break;
    }
} // validateExprTkExpression

KnobHelper::ExpressionReturnValueTypeEnum
KnobHelper::executeExprTkExpression(TimeValue time,
                                    ViewIdx view,
                                    DimIdx dimension,
                                    double* retValueIsScalar,
                                    string* retValueIsString,
                                    string* error)
{
    shared_ptr<KnobExprExprTk> obj;

    // Take the expression mutex. Copying the exprtk expression does not actually copy all variables and functions, it just
    // increments a shared reference count.
    // To be thread safe we have 2 solutions:
    // 1) Compile the expression for each thread and then run it without a mutex
    // 2) Compile only once and run the expression under a lock
    // We picked solution 1)
    {
        QMutexLocker k(&_imp->common->expressionMutex);
        ExprPerViewMap::const_iterator foundView = _imp->common->expressions[dimension].find(view);
        if ( ( foundView == _imp->common->expressions[dimension].end() ) || !foundView->second ) {
            return eExpressionReturnValueTypeError;
        }
        KnobExprExprTk* isExprtkExpr = dynamic_cast<KnobExprExprTk*>( foundView->second.get() );
        assert(isExprtkExpr);
        // Copy the expression object so it is local to this thread
        obj = boost::dynamic_pointer_cast<KnobExprExprTk>(foundView->second);
        assert(obj);
    }

    QThread* curThread = QThread::currentThread();

    KnobExprExprTk::ExpressionDataPtr data;
    {
        QMutexLocker k(&obj->lock);
        KnobExprExprTk::PerThreadDataMap::iterator foundThreadData = obj->data.find(curThread);
        if ( foundThreadData == obj->data.end() ) {
            data = KnobExprExprTk::createData();
            pair<KnobExprExprTk::PerThreadDataMap::iterator, bool> ret = obj->data.insert( make_pair(curThread, data) );
            assert(ret.second);
            foundThreadData = ret.first;
        } else {
            data = foundThreadData->second;
            assert(data);
        }
    }

    bool isRenderClone = getHolder()->isRenderClone();
    exprtk_symbol_table_t* unknown_symbols_table = 0;//
    exprtk_symbol_table_t* symbol_table = 0; //obj->expressionObject->get_symbol_table(1);
    bool existingExpression = true;
    if (!data->expressionObject) {
        // We did not build the expression already
        existingExpression = false;
        data->expressionObject.reset(new exprtk_expression_t);
        exprtk_symbol_table_t tmpUnresolved;
        exprtk_symbol_table_t tmpResolved;
        data->expressionObject->register_symbol_table(tmpUnresolved);
        data->expressionObject->register_symbol_table(tmpResolved);
    }

    unknown_symbols_table = &data->expressionObject->get_symbol_table(0);
    symbol_table = &data->expressionObject->get_symbol_table(1);


    if (existingExpression) {
        // Update the frame & view in the know table
        symbol_table->variable_ref("frame") = (double)time;

        // Remove from the symbol table functions that hold a state, and re-add a new fresh local copy of them so that the state
        // is local to this thread.
        exprtk_ifunction_table_t functionsCopy;
        makeLocalCopyOfStateFunctions(time, *symbol_table, &functionsCopy);
    } else {
        double time_f = (double)time;
        symbol_table->add_variable("frame", time_f);
        string viewName = getHolder()->getApp()->getProject()->getViewName(view);
        symbol_table->add_stringvar("view", viewName, false);

        addStandardFunctions(obj->expressionString, time, *symbol_table, data->functions, data->varargFunctions, data->genericFunctions, 0);


        KnobIPtr thisShared = shared_from_this();
        exprtk_igeneric_function_ptr curveFunc( new curve_func(thisShared, view) );
        data->genericFunctions.push_back( make_pair("curve", curveFunc) );
        symbol_table->add_function("curve", *curveFunc);

        exprtk_parser_t parser;
        string error;
        if ( !parseExprtkExpression(obj->expressionString, obj->modifiedExpression, parser, *data->expressionObject, &error) ) {
            return KnobHelper::eExpressionReturnValueTypeError;
        }
    } // existingExpression

    for (std::map<string, KnobDimViewKey>::const_iterator it = obj->knobDependencies.begin(); it != obj->knobDependencies.end(); ++it) {
        KnobIPtr knob = it->second.knob.lock();
        if (!knob) {
            continue;
        }

        if (isRenderClone) {
            // Get the render clone for this knob
            // First ensure a clone is created for the effect holding the knob
            // and then fetch the knob clone on it
            TreeRenderPtr render = getHolder()->getCurrentRender();
            assert(render);
            TimeValue time = getHolder()->getCurrentRenderTime();
            ViewIdx view = getHolder()->getCurrentRenderView();
            FrameViewRenderKey key = {time, view, render};
            KnobHolderPtr holderClone = knob->getHolder()->createRenderClone(key);

            knob = knob->getCloneForHolderInternal(holderClone);
        }

        KnobBoolBasePtr isBoolean = toKnobBoolBase(knob);
        KnobStringBasePtr isString = toKnobStringBase(knob);
        KnobIntBasePtr isInt = toKnobIntBase(knob);
        KnobDoubleBasePtr isDouble = toKnobDoubleBase(knob);
        if (existingExpression) {
            if (isBoolean) {
                unknown_symbols_table->variable_ref(it->first) = isBoolean->getValueAtTime(time, it->second.dimension, it->second.view);
            } else if (isInt) {
                unknown_symbols_table->variable_ref(it->first) = isInt->getValueAtTime(time, it->second.dimension, it->second.view);
            } else if (isDouble) {
                double val = isDouble->getValueAtTime(time, it->second.dimension, it->second.view);
                unknown_symbols_table->variable_ref(it->first) = val;
            } else if (isString) {
                unknown_symbols_table->stringvar_ref(it->first) = isString->getValueAtTime(time, it->second.dimension, it->second.view);
            }
        } else {
            if (isBoolean) {
                double value = (double)isBoolean->getValueAtTime(time, it->second.dimension, it->second.view);
                unknown_symbols_table->add_variable(it->first, value);
            } else if (isInt) {
                double value = (double)isInt->getValueAtTime(time, it->second.dimension, it->second.view);;
                unknown_symbols_table->add_variable(it->first, value);
            } else if (isDouble) {
                double val = isDouble->getValueAtTime(time, it->second.dimension, it->second.view);
                unknown_symbols_table->add_variable(it->first, val);
            } else if (isString) {
                string val = isString->getValueAtTime(time, it->second.dimension, it->second.view);
                unknown_symbols_table->add_stringvar(it->first, val);
            }
        }
    }

    for (std::map<string, EffectFunctionDependency>::const_iterator it = obj->effectDependencies.begin(); it != obj->effectDependencies.end(); ++it) {
        EffectInstancePtr effect = it->second.effect.lock();
        if (!effect) {
            continue;
        }

        if (isRenderClone) {
            // Get the render clone
            TreeRenderPtr render = getHolder()->getCurrentRender();
            assert(render);
            TimeValue time = getHolder()->getCurrentRenderTime();
            ViewIdx view = getHolder()->getCurrentRenderView();
            FrameViewRenderKey key = {time, view, render};
            effect = toEffectInstance( effect->createRenderClone(key) );
            assert( effect && effect->isRenderClone() );
        }
        switch (it->second.type) {
        case EffectFunctionDependency::eEffectFunctionDependencyRoD: {
            GetRegionOfDefinitionResultsPtr results;
            ActionRetCodeEnum stat = effect->getRegionOfDefinition_public(time, RenderScale(1.), view, &results);
            if ( isFailureRetCode(stat) ) {
                *error = it->first + ": Could not get region of definition";

                return eExpressionReturnValueTypeError;
            }
            const RectD& rod = results->getRoD();

            if (existingExpression) {
                exprtk_symbol_table_t::vector_holder_ptr vecHolderPtr = unknown_symbols_table->get_vector(it->first);
                assert(vecHolderPtr->size() == 4);
                *(*vecHolderPtr)[0] = exprtk_scalar_t(rod.x1);
                *(*vecHolderPtr)[1] = exprtk_scalar_t(rod.y1);
                *(*vecHolderPtr)[2] = exprtk_scalar_t(rod.x2);
                *(*vecHolderPtr)[3] = exprtk_scalar_t(rod.y2);
            } else {
                vector<double> vec(4);
                vec[0] = rod.x1;
                vec[1] = rod.y1;
                vec[2] = rod.x2;
                vec[3] = rod.y2;
                unknown_symbols_table->add_vector( it->first, &vec[0], vec.size() );
            }
            break;
        }
        }
    }


    // Evaluate the expression
    data->expressionObject->value();

    return handleExprTkReturn(*data->expressionObject, retValueIsScalar, retValueIsString, error);
} // executeExprTkExpression

KnobHelper::ExpressionReturnValueTypeEnum
KnobHelper::executeExprTkExpression(const string& expr,
                                    double* retValueIsScalar,
                                    string* retValueIsString,
                                    string* error)
{
    exprtk_symbol_table_t symbol_table;
    exprtk_expression_t expressionObj;

    expressionObj.register_symbol_table(symbol_table);

    exprtk_ifunction_table_t functions;
    exprtk_ivararg_function_table_t varargFunctions;
    exprtk_igeneric_function_table_t genericFunctions;
    TimeValue time(0);
    exprtk_parser_t parser;
    string modifiedExpr;
    addStandardFunctions(expr, time, symbol_table, functions, varargFunctions, genericFunctions, &modifiedExpr);

    if ( !parseExprtkExpression(expr, modifiedExpr, parser, expressionObj, error) ) {
        return eExpressionReturnValueTypeError;
    }

    // Evaluate the expression
    expressionObj.value();

    return handleExprTkReturn(expressionObj, retValueIsScalar, retValueIsString, error);
} // executeExprTkExpression

NATRON_NAMESPACE_EXIT
