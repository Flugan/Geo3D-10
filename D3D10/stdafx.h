// stdafx.h 
#pragma once


#define WIN32_LEAN_AND_MEAN		
#include <windows.h>
#include <share.h>
#include <stdio.h>
#include <direct.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "d3d10.h"

using namespace std;

struct VSO {
	ID3D10VertexShader* Left;
	ID3D10VertexShader* Neutral;
	ID3D10VertexShader* Right;
};

map<ID3D10VertexShader*, VSO> VSOmap;

typedef HRESULT(STDMETHODCALLTYPE* D3D10_VS)(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10VertexShader** ppVertexShader);
static struct {
	SIZE_T nHookId;
	D3D10_VS fnCreateVertexShader;
} sCreateVertexShader_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* D3D10_PS)(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10PixelShader** ppPixelShader);
static struct {
	SIZE_T nHookId;
	D3D10_PS fnCreatePixelShader;
} sCreatePixelShader_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* D3D10_GS)(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10GeometryShader** ppGeometryShader);
static struct {
	SIZE_T nHookId;
	D3D10_GS fnCreateGeometryShader;
} sCreateGeometryShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D10_PSSS)(ID3D10Device* This, ID3D10PixelShader* pPixelShader);
static struct {
	SIZE_T nHookId;
	D3D10_PSSS fnPSSetShader;
} sPSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D10_VSSS)(ID3D10Device* This, ID3D10VertexShader* pVertexShader);
static struct {
	SIZE_T nHookId;
	D3D10_VSSS fnVSSetShader;
} sVSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D10_GSSS)(ID3D10Device* This, ID3D10GeometryShader* pComputeShader);
static struct {
	SIZE_T nHookId;
	D3D10_GSSS fnGSSetShader;
} sGSSetShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* DXGI_Present)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
static struct {
	SIZE_T nHookId;
	DXGI_Present fnDXGI_Present;
} sDXGI_Present_Hook = { 0, NULL };

vector<byte> assembler(vector<byte> asmFile, vector<byte> buffer);
vector<byte> disassembler(vector<byte> buffer);
vector<byte> readFile(string fileName);
vector<string> stringToLines(const char* start, size_t size);
string shaderModel(byte* buffer);