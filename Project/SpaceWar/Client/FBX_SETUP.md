# FBX SDK 모델 로딩 설정

이 클라이언트는 Autodesk FBX SDK로 Arc Sentinel 모델을 읽습니다.
SDK는 저장소에 포함되어 있지 않습니다. 따라서 **소스를 빌드하는 각 PC에는 SDK 설치와 경로 설정이 필요합니다.**
`FBX SDK를 설치하고 ...` 오류는 이 조건이 충족되지 않았음을 알려주는 빌드 전 확인 메시지입니다.

## 개발 PC에서 한 번 설정

1. [Autodesk 공식 FBX SDK 페이지](https://aps.autodesk.com/developer/overview/fbx-sdk)에서 Windows용 C++ FBX SDK 2020.3 계열을 설치합니다. 교육과정에서 버전을 지정했다면 그 버전을 사용합니다. Python SDK나 FBX Viewer는 C++ SDK를 대신하지 않습니다.
2. 이 폴더의 `FbxSdk.local.props.example`을 같은 폴더에 `FbxSdk.local.props`라는 이름으로 복사합니다.
3. `FbxSdkRoot`를 실제 SDK 설치 경로로 바꿉니다. 지정한 폴더 안에 `include\fbxsdk.h`와 `lib` 폴더가 있어야 합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <FbxSdkRoot>C:\Program Files\Autodesk\FBX\FBX SDK\2020.3.11</FbxSdkRoot>
  </PropertyGroup>
</Project>
```

위 버전과 경로는 예시입니다. 설치한 위치가 다르면 반드시 바꿉니다.
`FbxSdk.local.props`는 Git에서 제외하므로 팀원마다 다른 위치에 설치해도 됩니다.
파일 설정 대신 사용자 환경 변수 `FBX_SDK_ROOT`에 같은 경로를 지정해도 됩니다. 환경 변수를 바꾼 뒤에는 Visual Studio를 다시 시작합니다.

`FbxSdk.props`는 다음 위치를 순서대로 사용합니다.

- `FbxSdk.local.props` 또는 MSBuild 속성의 `FbxSdkRoot`
- 환경 변수 `FBX_SDK_ROOT`
- 솔루션 폴더 아래 `external\fbxsdk`
- Program Files의 일반 설치 위치: 2020.3.11, 2020.3.9, 2020.3.7

SDK에 `lib\x64\debug` 및 `lib\x64\release` 구조가 있으면 해당 구성의 DLL과 import library를 확인해 우선 사용합니다. 현재 설치된 2020.3.11은 이 구조입니다.
이 구조가 없으면 `lib` 아래 compiler 폴더를 `vs2022`, `vs2019`, `vs2017` 순으로 선택합니다.
특정 폴더를 사용해야 한다면 개인 설정에 `<FbxSdkCompiler>vs2019</FbxSdkCompiler>`처럼 추가합니다.
실제 SDK가 다른 구조라면 `FbxSdkLibraryDir`을 구성별로 지정할 수 있습니다.

## 프로젝트에 적용된 설정

- FBX SDK 의존성은 Client에만 있습니다. Server와 Shared에는 연결하지 않습니다.
- x64 Debug는 SDK의 `debug` 폴더와 `/MDd`, Release는 `release` 폴더와 `/MD`를 사용합니다.
- DLL 방식으로 연결합니다: `FBXSDK_SHARED`, `libfbxsdk.lib`.
- 해당 구성의 `libfbxsdk.dll`을 빌드 출력 폴더로 자동 복사합니다.
- `Project/assets/model` 내용을 실행 파일 옆 `assets/model`로 자동 복사합니다.
- 모델 경로는 작업 디렉터리가 아니라 실행 파일 위치를 기준으로 계산합니다.

SDK의 `lib/x64/debug` 및 `release` 또는 `lib/<compiler>/x64/debug` 및 `release` 폴더에 각각 `libfbxsdk.lib`와 `libfbxsdk.dll`이 있는지 확인하세요.
Debug 라이브러리와 Release DLL을 섞지 않습니다.
설정 근거: [Autodesk Windows 연결 안내](https://help.autodesk.com/cloudhelp/2020/ENU/FBX-Developer-Help/files/getting_started/installing_and_configuring/FBX_Developer_Help_getting_started_installing_and_configuring_configuring_the_fbx_sdk_for_wind_html.html).

## 다른 PC에 게임을 전달할 때

완성된 게임을 실행하기만 하는 PC에는 FBX SDK 전체 설치가 필요하지 않습니다.
Release 출력 폴더의 `Client.exe`, `libfbxsdk.dll`, `dxcompiler.dll`, `dxil.dll`, `Shaders`, `assets`를 함께 전달합니다.
실행 PC에는 해당 MSVC 빌드 도구에 맞는 x64 Visual C++ 런타임도 필요합니다.
SDK 배포 시 Autodesk 배포 조건과 고지 파일을 함께 확인합니다.

내장 이미지는 사용자 임시 폴더의 `SpaceWar/FbxCache` 아래에 추출합니다.
원본 FBX나 실행 파일 옆의 assets 폴더에는 추출 파일을 쓰지 않습니다.
여러 클라이언트가 동시에 이미지를 추출해도 충돌하지 않도록 프로세스별로 경로를 나눕니다.

## 모델 처리 범위

- 대상: `Meshy_AI_01_Arc_Sentinel_0918131203_texture.fbx`
- 메시, 재질, 노드 계층과 내장 텍스처를 FBX SDK로 읽습니다.
- 기본 색상, 법선, 거칠기, 금속성, 발광 맵을 표시합니다.
- 이 파일은 거칠기 맵이 `ShininessExponent`, 금속성 맵이 `ReflectionFactor`에 연결되어 있어 텍스처 객체 이름과 함께 구분합니다.
- 텍스처는 WIC로 RGBA8로 읽고 GPU에 업로드합니다. 색상·발광만 sRGB로 해석합니다.
- Meshy/Blender의 +Y 법선맵을 기준으로 UV의 V 반전에 맞춰 법선맵 녹색 채널을 보정합니다.
- 정적 모델을 높이 2m로 맞추고, 기존 이동 중심보다 1m 아래에 발이 놓이도록 표시 노드를 보정합니다.
- 로컬 플레이어·원격 플레이어·NPC는 같은 GPU 메시와 재질을 공유합니다. 원격 모델의 정면은 위치 변화에서 추정합니다.
- 노드 이름과 계층을 보존하지만 뼈대 애니메이션·스키닝은 이번 구현에 포함하지 않습니다. 현재 FBX에도 뼈대와 애니메이션이 없습니다.

`Model.cpp`의 `kModelYaw`는 모델 정면 보정 각도(라디안)입니다. 기본값은 0이며 출력 결과를 보고 조정할 수 있습니다.
맵은 현재 한 단계의 텍스처 해상도로 업로드하며, 밉맵 생성과 투명 재질은 별도 확장 대상입니다.

## 사용자가 확인할 항목

이번 변경에서는 빌드·셰이더 컴파일·게임 실행을 수행하지 않았습니다.
SDK 설정 후 Debug/Release x64의 빌드와 실제 출력은 사용자가 확인합니다.

- 모델이 서 있는 방향, 높이와 발 위치, 이동·점프·회전
- 텍스처 방향, 기본 색상, 노멀맵, 거칠기·금속성·발광
- 기존 행성 지형과 카메라 동작
- 원격 플레이어/NPC의 생성·퇴장·재사용과 구면 위 직립
- 실행 파일 옆 DLL·셰이더·모델이 있는 상태에서 작업 디렉터리를 바꾸어 실행

SDK 설치 누락은 빌드 단계에서, FBX/이미지 로드 실패는 실행 중 메시지 상자에서 원인을 표시합니다.
