// proxydll.cpp
#include "stdafx.h"
#include "proxydll.h"
#include <Xinput.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "resource.h"
#include "..\Nektra\NktHookLib.h"
#include "..\vkeys.h"
#include "..\log.h"

// global variables
#pragma data_seg (".d3d10_shared")
HINSTANCE           gl_hOriginalDll = NULL;
HINSTANCE			gl_hThisInstance = NULL;
bool				gl_hookedDevice = false;
bool				gl_hookedContext = false;
bool				gl_dump = false;
bool				gl_log = false;
bool				gl_hunt = false;
bool				gl_cache_shaders = false;
bool				gl_left = false;
CRITICAL_SECTION	gl_CS;

ID3D10Texture2D* gStereoTextureLeft = NULL;
ID3D10ShaderResourceView* gStereoResourceViewLeft = NULL;
ID3D10Texture2D* gStereoTextureRight = NULL;
ID3D10ShaderResourceView* gStereoResourceViewRight = NULL;
ID3D10Texture1D* gIniTexture = NULL;
ID3D10ShaderResourceView* gIniResourceView = NULL;

float gSep;
float gConv;
float gEyeDist;
float gScreenSize;
float gFinalSep;

// Our parameters for the stereo parameters texture.
DirectX::XMFLOAT4	iniParams = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
FILE*				LogFile = NULL;
#pragma data_seg ()

CNktHookLib cHookMgr;

void log(char* s) {
	LogInfo("%s\n", s);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	bool result = true;

	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		gl_hThisInstance = hinstDLL;
		InitInstance();
		break;

	case DLL_PROCESS_DETACH:
		ExitInstance();
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;
	}

	return result;
}

// 64 bit magic FNV-0 and FNV-1 prime
#define FNV_64_PRIME ((UINT64)0x100000001b3ULL)
static UINT64 fnv_64_buf(const void *buf, size_t len)
{
	UINT64 hval = 0;
	unsigned const char *bp = (unsigned const char *)buf;	/* start of buffer */
	unsigned const char *be = bp + len;		/* beyond end of buffer */

	// FNV-1 hash each octet of the buffer
	while (bp < be) {
		// multiply by the 64 bit FNV magic prime mod 2^64 */
		hval *= FNV_64_PRIME;
		// xor the bottom with the current octet
		hval ^= (UINT64)*bp++;
	}
	return hval;
}

map<UINT64, bool> isCache;
map<UINT64, bool> hasStartPatch;
map<UINT64, bool> hasStartFix;

char cwd[MAX_PATH];

#pragma region Create
string changeASM(vector<byte> ASM, bool left) {
	auto lines = stringToLines((char*)ASM.data(), ASM.size());
	string shader;
	string oReg;
	bool dcl = false;
	bool dcl_ICB = false;
	int temp = 0;
	for (int i = 0; i < lines.size(); i++) {
		string s = lines[i];
		if (s.find("dcl") == 0) {
			dcl = true;
			dcl_ICB = false;
			if (s.find("dcl_output_siv") == 0 && s.find("position") != string::npos) {
				oReg = s.substr(15, 2);
				shader += s + "\n";
			}
			else if (s.find("dcl_temps") == 0) {
				string num = s.substr(10);
				temp = atoi(num.c_str()) + 2;
				shader += "dcl_temps " + to_string(temp) + "\n";
			}
			else if (s.find("dcl_immediateConstantBuffer") == 0) {
				dcl_ICB = true;
				shader += s + "\n";
			}
			else {
				shader += s + "\n";
			}
		}
		else if (dcl_ICB == true) {
			shader += s + "\n";
		}
		else if (dcl == true) {
			// after dcl
			if (s.find("ret") < s.size()) {
				char buf[80];
				sprintf_s(buf, 80, "%.8f", gFinalSep);
				string sep(buf);
				sprintf_s(buf, 80, "%.3f", gConv);
				string conv(buf);
				string changeSep = left ? "l(-" + sep + ")" : "l(" + sep + ")";
				shader +=
					"add r" + to_string(temp - 2) + ".x, r" + to_string(temp - 1) + ".w, l(-" + conv + ")\n" +
					"mad " + oReg + ".x, r" + to_string(temp - 2) + ".x, " + changeSep + ", r" + to_string(temp - 1) + ".x\n" +
					"ret\n";
			}
			if (oReg.size() == 0) {
				// no output
				return "";
			}
			if (temp == 0) {
				// add temps
				temp = 2;
				shader += "dcl_temps 2\n";
			}
			shader += s + "\n";
			auto pos = s.find(oReg);
			if (pos != string::npos) {
				string reg = "r" + to_string(temp - 1);
				for (int i = 0; i < s.size(); i++) {
					if (i < pos) {
						shader += s[i];
					}
					else if (i == pos) {
						shader += reg;
					}
					else if (i > pos + 1) {
						shader += s[i];
					}
				}
				shader += "\n";
			}
		}
		else {
			// before dcl
			shader += s + "\n";
		}
	}
	return shader;
}

void dump(const void* pShaderBytecode, SIZE_T BytecodeLength, char* buffer) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderCache");
	CreateDirectory(path, NULL);
	strcat_s(path, MAX_PATH, "\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".bin");
	EnterCriticalSection(&gl_CS);
	FILE* f;
	fopen_s(&f, path, "wb");
	fwrite(pShaderBytecode, 1, BytecodeLength, f);
	fclose(f);
	LeaveCriticalSection(&gl_CS);
}
vector<byte> assembled(char* buffer, const void* pShaderBytecode, SIZE_T BytecodeLength) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".txt");
	vector<byte> file = readFile(path);

	vector<byte> byteCode;

	vector<byte>* v = new vector<byte>(BytecodeLength);
	copy((byte*)pShaderBytecode, (byte*)pShaderBytecode + BytecodeLength, v->begin());

	if (byteCode.size() == 0) {
		byteCode = assembler(file, *v);
	}
	delete v;
	return byteCode;
}
ID3DBlob* hlsled(char* buffer, char* shdModel) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, "_replace.txt");
	vector<byte> file = readFile(path);

	ID3DBlob* pByteCode = nullptr;
	ID3DBlob* pErrorMsgs = nullptr;
	HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
		"main", shdModel, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
	return pByteCode;
}

HRESULT STDMETHODCALLTYPE D3D10_CreateVertexShader(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10VertexShader** ppVertexShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create VertexShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-vs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT hr;
	byte* bArray;
	SIZE_T bSize;
	if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		bArray = (byte*)data.data();
		bSize = data.size();
	}
	else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "vs_4_0");
		bArray = (byte*)pByteCode->GetBufferPointer();
		bSize = pByteCode->GetBufferSize();
	}
	else {
		bArray = (byte*)pShaderBytecode;
		bSize = BytecodeLength;
	}
	vector<byte> v;
	for (int i = 0; i < bSize; i++) {
		v.push_back(bArray[i]);
	}
	vector<byte> ASM = disassembler(v);

	string shaderL = changeASM(ASM, true);
	string shaderR = changeASM(ASM, false);

	if (shaderL == "") {
		hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, ppVertexShader);
		VSO vso = {};
		vso.Neutral = (ID3D10VertexShader*)*ppVertexShader;
		VSOmap[vso.Neutral] = vso;
		return hr;
	}

	vector<byte> a;
	VSO vso = {};

	a.clear();
	for (int i = 0; i < shaderL.length(); i++) {
		a.push_back(shaderL[i]);
	}
	auto compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, compiled.data(), compiled.size(), ppVertexShader);
	vso.Left = (ID3D10VertexShader*)*ppVertexShader;

	a.clear();
	for (int i = 0; i < shaderR.length(); i++) {
		a.push_back(shaderR[i]);
	}
	compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, compiled.data(), compiled.size(), ppVertexShader);
	vso.Right = (ID3D10VertexShader*)*ppVertexShader;
	VSOmap[vso.Right] = vso;
	return hr;
}

HRESULT STDMETHODCALLTYPE D3D10_CreatePixelShader(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10PixelShader** ppPixelShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create PixelShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-ps", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, data.data(), data.size(), ppPixelShader);
	}
	else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "ps_4_0");
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), ppPixelShader);
	}
	else {
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pShaderBytecode, BytecodeLength, ppPixelShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D10_CreateGeometryShader(ID3D10Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10GeometryShader** ppGeometryShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create GeometryShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-gs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, data.data(), data.size(), ppGeometryShader);
	}
	else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "gs_4_0");
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), ppGeometryShader);
	}
	else {
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pShaderBytecode, BytecodeLength, ppGeometryShader);
	}
	return res;
}
#pragma endregion

#pragma region SetShader
void STDMETHODCALLTYPE D3D10_PSSetShader(ID3D10Device * This, ID3D10PixelShader *pPixelShader) {
	sPSSetShader_Hook.fnPSSetShader(This, pPixelShader);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->PSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->PSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->PSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D10_VSSetShader(ID3D10Device * This, ID3D10VertexShader *pVertexShader) {
	if (VSOmap.count(pVertexShader) == 1) {
		VSO* vso = &VSOmap[pVertexShader];
		if (vso->Neutral) {
			LogInfo("No output VS\n");
			sVSSetShader_Hook.fnVSSetShader(This, vso->Neutral);
		}
		else {
			LogInfo("Stereo VS\n");
			if (gl_left) {
				sVSSetShader_Hook.fnVSSetShader(This, vso->Left);
			}
			else {
				sVSSetShader_Hook.fnVSSetShader(This, vso->Right);
			}
		}
	}
	else {
		LogInfo("Unknown VS\n");
		sVSSetShader_Hook.fnVSSetShader(This, pVertexShader);
	}
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->VSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->VSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->VSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D10_GSSetShader(ID3D10Device * This, ID3D10GeometryShader *pGeometryShader) {
	sGSSetShader_Hook.fnGSSetShader(This, pGeometryShader);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->GSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->GSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->GSSetShaderResources(120, 1, &gIniResourceView);
}
#pragma endregion

#pragma region hook
HRESULT CreateStereoParamTextureAndView(ID3D10Device* d3d10)
{
	HRESULT hr = 0;

	float eyeSep = gEyeDist / (2.54f * gScreenSize * 16 / sqrtf(256 + 81));

	const int StereoBytesPerPixel = 16;
	const int stagingWidth = 8;
	const int stagingHeight = 1;

	D3D10_SUBRESOURCE_DATA sysData;
	sysData.SysMemPitch = StereoBytesPerPixel * stagingWidth;
	sysData.pSysMem = new unsigned char[sysData.SysMemPitch * stagingHeight];
	float* leftEye = (float*)sysData.pSysMem;
	gFinalSep = eyeSep * gSep * 0.01f;
	leftEye[0] = -gFinalSep;
	leftEye[1] = gConv;
	leftEye[2] = 1.0f;

	D3D10_TEXTURE2D_DESC desc;
	desc.Width = stagingWidth;
	desc.Height = stagingHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D10_USAGE_DYNAMIC;
	desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
	desc.MiscFlags = 0;
	d3d10->CreateTexture2D(&desc, &sysData, &gStereoTextureLeft);
	LogInfo("StereoTexture: %d\n", gStereoTextureLeft > 0);

	float* rightEye = (float*)sysData.pSysMem;
	rightEye[0] = gFinalSep;
	rightEye[1] = gConv;
	rightEye[2] = -1.0f;

	d3d10->CreateTexture2D(&desc, &sysData, &gStereoTextureRight);

	delete[] sysData.pSysMem;

	// Since we need to bind the texture to a shader input, we also need a resource view.
	D3D10_SHADER_RESOURCE_VIEW_DESC descRV;
	descRV.Format = desc.Format;
	descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels = 1;
	descRV.Texture2D.MostDetailedMip = 0;
	descRV.Texture2DArray.MostDetailedMip = 0;
	descRV.Texture2DArray.MipLevels = 1;
	descRV.Texture2DArray.FirstArraySlice = 0;
	descRV.Texture2DArray.ArraySize = desc.ArraySize;
	d3d10->CreateShaderResourceView(gStereoTextureLeft, &descRV, &gStereoResourceViewLeft);

	d3d10->CreateShaderResourceView(gStereoTextureRight, &descRV, &gStereoResourceViewRight);

	return S_OK;
}

HRESULT STDMETHODCALLTYPE hDXGI_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
	LogInfo("Present\n");
	gl_left = !gl_left;
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

void InitializeStereo(ID3D10Device* pDevice) {
	// Create our stereo parameter texture
	CreateStereoParamTextureAndView(pDevice);
	gl_left = true;
	LogInfo("Stereo Initialized\n");
}

void hook(ID3D10Device** ppDevice) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Hook device: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;
			
			D3D10_VS origVS = (D3D10_VS)(*vTable)[79];
			D3D10_PS origPS = (D3D10_PS)(*vTable)[82];
			D3D10_GS origGS = (D3D10_GS)(*vTable)[80];

			D3D10_VSSS origVSSS = (D3D10_VSSS)(*vTable)[7];
			D3D10_PSSS origPSSS = (D3D10_PSSS)(*vTable)[5];
			D3D10_GSSS origGSSS = (D3D10_GSSS)(*vTable)[17];

			cHookMgr.Hook(&(sPSSetShader_Hook.nHookId), (LPVOID*)&(sPSSetShader_Hook.fnPSSetShader), origPSSS, D3D10_PSSetShader);
			cHookMgr.Hook(&(sVSSetShader_Hook.nHookId), (LPVOID*)&(sVSSetShader_Hook.fnVSSetShader), origVSSS, D3D10_VSSetShader);
			cHookMgr.Hook(&(sGSSetShader_Hook.nHookId), (LPVOID*)&(sGSSetShader_Hook.fnGSSetShader), origGSSS, D3D10_GSSetShader);

			cHookMgr.Hook(&(sCreatePixelShader_Hook.nHookId), (LPVOID*)&(sCreatePixelShader_Hook.fnCreatePixelShader), origPS, D3D10_CreatePixelShader);
			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fnCreateVertexShader), origVS, D3D10_CreateVertexShader);
			cHookMgr.Hook(&(sCreateGeometryShader_Hook.nHookId), (LPVOID*)&(sCreateGeometryShader_Hook.fnCreateGeometryShader), origGS, D3D10_CreateGeometryShader);

			IDXGIFactory1 * pFactory;
			HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));

			// Temp window
			HWND dummyHWND = ::CreateWindow("STATIC", "dummy", WS_DISABLED, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
			::SetWindowTextA(dummyHWND, "Dummy Window!");

			// create a struct to hold information about the swap chain
			DXGI_SWAP_CHAIN_DESC scd;

			// clear out the struct for use
			ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

			// fill the swap chain description struct
			scd.BufferCount = 1;									// one back buffer
			scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;		// use 32-bit color
			scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;		// how swap chain is to be used
			scd.OutputWindow = dummyHWND;							// the window to be used
			scd.SampleDesc.Count = 1;								// how many multisamples
			scd.Windowed = TRUE;									// windowed/full-screen mode

			IDXGISwapChain * pSC;

			pFactory->CreateSwapChain(*ppDevice, &scd, &pSC);

			DWORD_PTR*** vTable2 = (DWORD_PTR***)pSC;
			DXGI_Present origPresent = (DXGI_Present)(*vTable2)[8];

			pSC->Release();
			pFactory->Release();
			::DestroyWindow(dummyHWND);

			cHookMgr.Hook(&(sDXGI_Present_Hook.nHookId), (LPVOID*)&(sDXGI_Present_Hook.fnDXGI_Present), origPresent, hDXGI_Present);

			gl_hookedDevice = true;
		}
		InitializeStereo(*ppDevice);
	}
}

// Exported function (faking d3d10.dll's export)
HRESULT WINAPI D3D10CreateDevice(IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, ID3D10Device **ppDevice) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, ID3D10Device **ppDevice);
	D3D10_Type D3D11CreateDevice_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateDevice");
	HRESULT res = D3D11CreateDevice_fn(pAdapter, DriverType, Software, Flags, SDKVersion, ppDevice);
	if (!FAILED(res)) {
		hook(ppDevice);
	}
	return res;
}

HRESULT WINAPI D3D10CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, 
												const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D10Device **ppDevice) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, INT Flags, UINT SDKVersion,
											const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D10Device **ppDevice);
	D3D10_Type D3D10CreateDeviceAndSwapChain_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateDeviceAndSwapChain");
	HRESULT res = D3D10CreateDeviceAndSwapChain_fn(pAdapter, DriverType, Software, Flags, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice);
	if (!FAILED(res)) {
		hook(ppDevice);
	}
	return res;
}
#pragma endregion

void InitInstance()
{
	// Initialisation
	char setting[MAX_PATH];
	char INIfile[MAX_PATH];
	char LOGfile[MAX_PATH];

	InitializeCriticalSection(&gl_CS);

	_getcwd(INIfile, MAX_PATH);
	_getcwd(LOGfile, MAX_PATH);
	strcat_s(INIfile, MAX_PATH, "\\d3dx.ini");
	_getcwd(cwd, MAX_PATH);

	// If specified in Debug section, wait for Attach to Debugger.
	bool waitfordebugger = GetPrivateProfileInt("Debug", "attach", 0, INIfile) > 0;
	if (waitfordebugger) {
		do {
			Sleep(250);
		} while (!IsDebuggerPresent());
	}

	gl_dump = GetPrivateProfileInt("Rendering", "export_binary", gl_dump, INIfile) > 0;
	gl_log = GetPrivateProfileInt("Logging", "calls", gl_log, INIfile) > 0;

	if (GetPrivateProfileString("StereoSettings", "StereoSeparation", "50", setting, MAX_PATH, INIfile)) {
		gSep = stof(setting);
	}
	if (GetPrivateProfileString("StereoSettings", "StereoConvergence", "1.0", setting, MAX_PATH, INIfile)) {
		gConv = stof(setting);
	}
	if (GetPrivateProfileString("StereoSettings", "EyeDistance", "6.3", setting, MAX_PATH, INIfile)) {
		gEyeDist = stof(setting);
	}
	if (GetPrivateProfileString("StereoSettings", "ScreenSize", "27", setting, MAX_PATH, INIfile)) {
		gScreenSize = stof(setting);
	}

	if (gl_log) {
		if (LogFile == NULL) {
			strcat_s(LOGfile, MAX_PATH, "\\d3d10_log.txt");
			LogFile = _fsopen(LOGfile, "w", _SH_DENYNO);
			setvbuf(LogFile, NULL, _IONBF, 0);
		}
	}

	WIN32_FIND_DATA findFileData;

	HANDLE hFind = FindFirstFile("ShaderFixes\\????????????????-??.bin", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			isCache[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartPatch[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??_replace.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartFix[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}
	LogInfo("ini loaded:\n");
}

void LoadOriginalDll(void)
{
	wchar_t sysDir[MAX_PATH];
	::GetSystemDirectoryW(sysDir, MAX_PATH);
	wcscat_s(sysDir, MAX_PATH, L"\\d3d10.dll");
	if (!gl_hOriginalDll) gl_hOriginalDll = ::LoadLibraryExW(sysDir, NULL, NULL);
}

void ExitInstance()
{
	if (gl_hOriginalDll)
	{
		::FreeLibrary(gl_hOriginalDll);
		gl_hOriginalDll = NULL;
	}
}

HRESULT WINAPI D3D10ReflectShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10ShaderReflection **ppReflector) {
	log("D3D10ReflectShader");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10ShaderReflection **ppReflector);
	D3D10_Type D3D10ReflectShader_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10ReflectShader");
	return D3D10ReflectShader_fn(pShaderBytecode, BytecodeLength, ppReflector);
}

HRESULT WINAPI D3D10CompileEffectFromMemory(void *pData, SIZE_T DataLength, LPCSTR pSrcFileName, D3D10_SHADER_MACRO *pDefines, ID3D10Include *pInclude, UINT HLSLFlags, UINT FXFlags, ID3D10Blob **ppCompiledEffect, ID3D10Blob **ppErrors) {
	log("D3D10CompileEffectFromMemory");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(void *pData, SIZE_T DataLength, LPCSTR pSrcFileName, D3D10_SHADER_MACRO *pDefines, ID3D10Include *pInclude, UINT HLSLFlags, UINT FXFlags, ID3D10Blob **ppCompiledEffect, ID3D10Blob **ppErrors);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CompileEffectFromMemory");
	return orig_fn(pData, DataLength, pSrcFileName, pDefines, pInclude, HLSLFlags, FXFlags, ppCompiledEffect, ppErrors);
}

HRESULT WINAPI D3D10CompileShader(LPCSTR pSrcData, SIZE_T SrcDataLen, LPCSTR pFileName, const D3D10_SHADER_MACRO *pDefines, LPD3D10INCLUDE *pInclude, LPCSTR pFunctionName, LPCSTR pProfile, UINT Flags, ID3D10Blob **ppShader, ID3D10Blob **ppErrorMsgs) {
	log("D3D10CompileShader");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(LPCSTR pSrcData, SIZE_T SrcDataLen, LPCSTR pFileName, const D3D10_SHADER_MACRO *pDefines, LPD3D10INCLUDE *pInclude, LPCSTR pFunctionName, LPCSTR pProfile, UINT Flags, ID3D10Blob **ppShader, ID3D10Blob **ppErrorMsgs);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CompileShader");
	return orig_fn(pSrcData, SrcDataLen, pFileName, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs);
}

HRESULT WINAPI D3D10CreateBlob(SIZE_T NumBytes, LPD3D10BLOB *ppBuffer) {
	log("D3D10CreateBlob");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(SIZE_T NumBytes, LPD3D10BLOB *ppBuffer);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateBlob");
	return orig_fn(NumBytes, ppBuffer);
}

HRESULT WINAPI D3D10CreateEffectFromMemory(void *pData, SIZE_T DataLength, UINT FXFlags, ID3D10Device *pDevice, ID3D10EffectPool *pEffectPool, ID3D10Effect **ppEffect) {
	log("D3D10CreateEffectFromMemory");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(void *pData, SIZE_T DataLength, UINT FXFlags, ID3D10Device *pDevice, ID3D10EffectPool *pEffectPool, ID3D10Effect **ppEffect);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateEffectFromMemory");
	return orig_fn(pData, DataLength, FXFlags, pDevice, pEffectPool, ppEffect);
}

HRESULT WINAPI D3D10CreateEffectPoolFromMemory(void *pData, SIZE_T DataLength, UINT FXFlags, ID3D10Device *pDevice, ID3D10EffectPool **ppEffectPool) {
	log("D3D10CreateEffectPoolFromMemory");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(void *pData, SIZE_T DataLength, UINT FXFlags, ID3D10Device *pDevice, ID3D10EffectPool **ppEffectPool);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateEffectPoolFromMemory");
	return orig_fn(pData, DataLength, FXFlags, pDevice, ppEffectPool);
}

HRESULT WINAPI D3D10CreateStateBlock(ID3D10Device *pDevice, D3D10_STATE_BLOCK_MASK *pStateBlockMask, ID3D10StateBlock **ppStateBlock) {
	log("D3D10CreateStateBlock");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(ID3D10Device *pDevice, D3D10_STATE_BLOCK_MASK *pStateBlockMask, ID3D10StateBlock **ppStateBlock);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10CreateStateBlock");
	return orig_fn(pDevice, pStateBlockMask, ppStateBlock);
}

HRESULT WINAPI D3D10DisassembleEffect(ID3D10Effect *pEffect, BOOL EnableColorCode, ID3D10Blob **ppDisassembly) {
	log("D3D10DisassembleEffect");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(ID3D10Effect *pEffect, BOOL EnableColorCode, ID3D10Blob **ppDisassembly);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10DisassembleEffect");
	return orig_fn(pEffect, EnableColorCode, ppDisassembly);
}

HRESULT WINAPI D3D10DisassembleShader(const void *pShader, SIZE_T BytecodeLength, BOOL EnableColorCode, LPCSTR pComments, ID3D10Blob **ppDisassembly) {
	log("D3D10DisassembleShader");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShader, SIZE_T BytecodeLength, BOOL EnableColorCode, LPCSTR pComments, ID3D10Blob **ppDisassembly);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10DisassembleShader");
	return orig_fn(pShader, BytecodeLength, EnableColorCode, pComments, ppDisassembly);
}

LPCSTR WINAPI D3D10GetGeometryShaderProfile(ID3D10Device *pDevice) {
	log("D3D10GetGeometryShaderProfile");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef LPCSTR(WINAPI* D3D10_Type)(ID3D10Device *pDevice);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetGeometryShaderProfile");
	return orig_fn(pDevice);
}

HRESULT WINAPI D3D10GetInputAndOutputSignatureBlob(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob) {
	log("D3D10GetInputAndOutputSignatureBlob");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetInputAndOutputSignatureBlob");
	return orig_fn(pShaderBytecode, BytecodeLength, ppSignatureBlob);
}

HRESULT WINAPI D3D10GetInputSignatureBlob(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob) {
	log("D3D10GetInputSignatureBlob");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetInputSignatureBlob");
	return orig_fn(pShaderBytecode, BytecodeLength, ppSignatureBlob);
}

HRESULT WINAPI D3D10GetOutputSignatureBlob(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob) {
	log("D3D10GetOutputSignatureBlob");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppSignatureBlob);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetOutputSignatureBlob");
	return orig_fn(pShaderBytecode, BytecodeLength, ppSignatureBlob);
}

LPCSTR WINAPI D3D10GetPixelShaderProfile(ID3D10Device *pDevice) {
	log("D3D10GetPixelShaderProfile");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef LPCSTR(WINAPI* D3D10_Type)(ID3D10Device *pDevice);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetPixelShaderProfile");
	return orig_fn(pDevice);
}

HRESULT WINAPI D3D10GetShaderDebugInfo(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppDebugInfo) {
	log("D3D10GetShaderDebugInfo");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D10Blob **ppDebugInfo);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetShaderDebugInfo");
	return orig_fn(pShaderBytecode, BytecodeLength, ppDebugInfo);
}

LPCSTR WINAPI D3D10GetVertexShaderProfile(ID3D10Device *pDevice) {
	log("D3D10GetVertexShaderProfile");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef LPCSTR(WINAPI* D3D10_Type)(ID3D10Device *pDevice);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10GetVertexShaderProfile");
	return orig_fn(pDevice);
}

HRESULT WINAPI D3D10PreprocessShader(LPCSTR pSrcData, SIZE_T SrcDataSize, LPCSTR pFileName, const D3D10_SHADER_MACRO *pDefines, LPD3D10INCLUDE pInclude, ID3D10Blob **ppShaderText, ID3D10Blob **ppErrorMsgs) {
	log("D3D10PreprocessShader");
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d10.dll"
	typedef HRESULT(WINAPI* D3D10_Type)(LPCSTR pSrcData, SIZE_T SrcDataSize, LPCSTR pFileName, const D3D10_SHADER_MACRO *pDefines, LPD3D10INCLUDE pInclude, ID3D10Blob **ppShaderText, ID3D10Blob **ppErrorMsgs);
	D3D10_Type orig_fn = (D3D10_Type)GetProcAddress(gl_hOriginalDll, "D3D10PreprocessShader");
	return orig_fn(pSrcData, SrcDataSize, pFileName, pDefines, pInclude, ppShaderText, ppErrorMsgs);
}