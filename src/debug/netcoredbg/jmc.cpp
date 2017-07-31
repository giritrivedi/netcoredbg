#include "common.h"

#include <string>
#include <vector>
#include <list>

#include "typeprinter.h"
#include "platform.h"
#include "symbolreader.h"

static const char *g_nonUserCode = "System.Diagnostics.DebuggerNonUserCodeAttribute..ctor";
static const char *g_stepThrough = "System.Diagnostics.DebuggerStepThroughAttribute..ctor";
// TODO: DebuggerStepThroughAttribute also affects breakpoints when JMC is enabled

bool ShouldLoadSymbolsForModule(const std::string &moduleName)
{
    std::string name = GetFileName(moduleName);
    if (name.find("System.") == 0 || name.find("SOS.") == 0)
        return false;
    return true;
}

static bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const std::string &attrName)
{
    bool found = false;

    ULONG numAttributes = 0;
    HCORENUM fEnum = NULL;
    mdCustomAttribute attr;
    while(SUCCEEDED(pMD->EnumCustomAttributes(&fEnum, tok, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
    {
        mdToken ptkObj = mdTokenNil;
        mdToken ptkType = mdTokenNil;
        pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, nullptr, nullptr);

        std::string mdName;
        std::list<std::string> emptyArgs;
        TypePrinter::NameForToken(ptkType, pMD, mdName, true, emptyArgs);

        if (mdName == attrName)
        {
            found = true;
            break;
        }
    }
    pMD->CloseEnum(fEnum);

    return found;
}

static bool HasSourceLocation(SymbolReader *symbolReader, mdMethodDef methodDef)
{
    HRESULT Status;
    std::vector<SymbolReader::SequencePoint> points;
    if (FAILED(symbolReader->GetSequencePoints(methodDef, points)))
        return false;

    for (auto &p : points)
    {
        if (p.startLine != 0 && p.startLine != SymbolReader::HiddenLine)
            return true;
    }
    return false;
}

static HRESULT GetNonJMCMethodsForTypeDef(
    IMetaDataImport *pMD,
    SymbolReader *sr,
    mdTypeDef typeDef,
    std::vector<mdToken> &excludeMethods)
{
    HRESULT Status;

    ULONG numMethods = 0;
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef;
    while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {
        HRESULT hr;
        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};

        Status = pMD->GetMethodProps(methodDef, &memTypeDef,
                                     szFunctionName, _countof(szFunctionName), &nameLen,
                                     nullptr, nullptr, nullptr, nullptr, nullptr);

        if (HasAttribute(pMD, methodDef, g_nonUserCode))
            excludeMethods.push_back(methodDef);
        else if (HasAttribute(pMD, methodDef, g_stepThrough))
            excludeMethods.push_back(methodDef);
        else if (!HasSourceLocation(sr, methodDef))
            excludeMethods.push_back(methodDef);
    }
    pMD->CloseEnum(fEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdMethodDef mdSetter;
        mdMethodDef mdGetter;
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef,
                                            nullptr,
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            &mdSetter,
                                            &mdGetter,
                                            nullptr,
                                            0,
                                            nullptr)))
        {
            if (mdSetter != mdMethodDefNil)
                excludeMethods.push_back(mdSetter);
            if (mdGetter != mdMethodDefNil)
                excludeMethods.push_back(mdGetter);
        }
    }
    pMD->CloseEnum(propEnum);

    return S_OK;
}

static HRESULT GetNonJMCClassesAndMethods(ICorDebugModule *pModule, SymbolReader *sr, std::vector<mdToken> &excludeTokens)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numTypedefs = 0;
    HCORENUM fEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMD->EnumTypeDefs(&fEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        if (HasAttribute(pMD, typeDef, g_nonUserCode))
            excludeTokens.push_back(typeDef);
        else
            GetNonJMCMethodsForTypeDef(pMD, sr, typeDef, excludeTokens);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

HRESULT SetJMCFromAttributes(ICorDebugModule *pModule, SymbolReader *symbolReader)
{
    std::vector<mdToken> excludeTokens;

    GetNonJMCClassesAndMethods(pModule, symbolReader, excludeTokens);

    for (mdToken token : excludeTokens)
    {
        if (TypeFromToken(token) == mdtMethodDef)
        {
            ToRelease<ICorDebugFunction> pFunction;
            ToRelease<ICorDebugFunction2> pFunction2;
            if (FAILED(pModule->GetFunctionFromToken(token, &pFunction)))
                continue;
            if (FAILED(pFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID *)&pFunction2)))
                continue;

            pFunction2->SetJMCStatus(FALSE);
        }
        else if (TypeFromToken(token) == mdtTypeDef)
        {
            ToRelease<ICorDebugClass> pClass;
            ToRelease<ICorDebugClass2> pClass2;
            if (FAILED(pModule->GetClassFromToken(token, &pClass)))
                continue;
            if (FAILED(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID *)&pClass2)))
                continue;

            pClass2->SetJMCStatus(FALSE);
        }
    }

    return S_OK;
}
