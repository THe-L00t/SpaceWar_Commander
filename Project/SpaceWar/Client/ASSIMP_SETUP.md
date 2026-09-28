# Assimp 모델 로딩 준비

Client는 Assimp로 `assets/model`의 FBX를 읽습니다. Assimp 설치 프로그램이나 PC별 헤더·라이브러리 경로 입력은 필요하지 않습니다. 프로젝트의 `vcpkg.json`을 이용해 의존성을 준비합니다.

## 다른 PC에서 빌드하기

1. Visual Studio Installer에서 **C++를 사용한 데스크톱 개발**, **MSVC v143 x64/x86 빌드 도구**, **Windows SDK**, **vcpkg 패키지 관리자**를 준비합니다. Visual Studio에 이미 설치되어 있다면 추가 설치하지 않아도 됩니다.
2. GitHub Desktop으로 프로젝트를 받아 솔루션을 엽니다. 모델과 지형 등 `assets`도 함께 받아야 합니다.
3. 구성은 **Debug | x64** 또는 **Release | x64**, 시작 프로젝트는 **Client**로 선택합니다.
4. 사용자가 빌드하면 vcpkg가 필요한 Assimp 및 의존성을 다운로드하고 빌드합니다. 첫 빌드에는 인터넷 연결이 필요하며 라이브러리 준비 때문에 시간이 더 걸릴 수 있습니다.

이 구성은 라이브러리 바이너리를 Git에 포함하는 방식이 아닙니다. 다른 PC의 첫 빌드에서는 자동 다운로드가 발생합니다. 재빌드할 때는 설치된 패키지와 vcpkg 캐시를 재사용합니다. 아직 패키지를 준비하지 않았다면 IntelliSense에 Assimp 헤더 오류가 표시될 수 있으며, 의존성 설치가 끝난 뒤 프로젝트를 다시 읽으면 됩니다.

## 프로젝트 설정

| 파일 | 역할 |
|---|---|
| `Assimp.props` | Visual Studio에 포함된 vcpkg 탐색, Client 매니페스트와 `x64-windows` 설정, 로컬 의존성 작업 폴더 지정 |
| `Assimp.targets` | vcpkg 연동 확인, Debug `/MDd` 및 Release `/MD` 설정 |
| `vcpkg.json` | Assimp **6.0.4#2**와 의존성 기준 커밋 고정 |
| `vcpkg-configuration.json`, `vcpkg-triplets/x64-windows.cmake` | 라이브러리도 Client와 같은 v143 도구 모음으로 빌드하도록 지정 |
| `$(AssimpDependencyRoot)/installed/` | 각 PC의 로컬 작업 폴더에 생성되는 헤더·라이브러리·DLL |

`Assimp.props`는 `Microsoft.Cpp.props`보다 먼저, `Assimp.targets`는 `Microsoft.Cpp.targets` 다음에 등록되어 있습니다. C++ 프로젝트의 `VCLibPackagePath` 확장 지점을 통해 vcpkg의 공식 MSBuild 파일을 읽으며, 별도 `vcpkg integrate install` 명령은 필요하지 않습니다. 매니페스트는 Client 폴더에만 두므로 Shared와 Server에는 Assimp 의존성을 추가하지 않습니다.

vcpkg가 Debug/Release에 맞는 헤더와 라이브러리를 연결하고, 실행에 필요한 Assimp 및 관련 DLL을 실행 파일 폴더에 복사합니다. 기존 셰이더·모델·지형 복사 과정은 프로젝트 설정을 사용합니다.

Visual Studio가 여러 버전 설치되어 있어도 의존성에는 v143을 사용합니다. 추후 Client의 `PlatformToolset`을 변경한다면 위 triplet의 `VCPKG_PLATFORM_TOOLSET`도 함께 변경합니다.

## 의존성 작업 폴더

기본 작업 폴더는 `%LOCALAPPDATA%\SpaceWar\vcpkg\<프로젝트 경로 해시>`입니다. Visual Studio 빌드 출력의 **Assimp 의존성 작업 폴더**에서 실제 경로를 확인할 수 있습니다. 프로젝트마다 경로를 구분하며 Debug와 Release는 같은 작업 폴더를 사용합니다.

- `installed/`: 설치된 헤더·라이브러리·DLL
- `buildtrees/`: 압축을 푼 소스, 중간 파일, 패키지별 로그
- `packages/`: 설치 전 패키지 임시 파일

이 설정은 프로젝트 전체를 이동하지 않고 라이브러리 작업 경로를 OneDrive 밖으로 분리합니다. 이 PC의 기본 경로는 한글과 공백을 포함하지 않습니다. Windows 사용자 폴더에 한글이나 공백이 있는 PC에서는 사용자 환경 변수 `AssimpDependencyRoot`를 쓰기 가능한 짧은 영문 경로(예: `C:\dev\SpaceWar-deps`)로 지정하고 Visual Studio를 다시 엽니다. 직접 지정한 경로는 이 프로젝트 전용으로 사용합니다.

다음 빌드에서 새 작업 폴더에 의존성을 준비합니다. 기존 다운로드·바이너리 캐시는 계속 사용할 수 있으며, 이전 `Client/vcpkg_installed/`의 파일은 자동으로 이동하거나 삭제하지 않습니다. 프로젝트를 다른 경로에 복사하면 경로 해시도 바뀌므로 별도의 작업 폴더가 사용됩니다.

현재 로그에서는 zlib 소스 준비 중 CMake가 `3221226505 (0xC0000409)`로 비정상 종료했습니다. 한글·공백 경로가 직접 원인인지는 확정되지 않았으며, 작업 폴더 분리는 경로 영향을 피하기 위한 조치입니다. 적용 후 빌드 성공 여부는 사용자가 확인합니다.

작업 폴더 지정에는 vcpkg의 공식 [MSBuild 설정](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/msbuild-integration)과 [경로 지정 옵션](https://learn.microsoft.com/en-us/vcpkg/commands/common-options)을 사용합니다.

## vcpkg를 찾지 못하는 경우

기본적으로 현재 Visual Studio의 `VC/vcpkg` 폴더를 탐색합니다. 없다면 Visual Studio Installer의 개별 구성 요소에서 **vcpkg 패키지 관리자**를 추가합니다.

별도로 준비한 vcpkg를 쓰려면 Windows 사용자 환경 변수 `VCPKG_ROOT`를 `vcpkg.exe`가 있는 폴더로 지정하고 Visual Studio를 완전히 종료한 뒤 다시 엽니다. 예: `C:\dev\vcpkg`. 해당 폴더에는 `scripts/buildsystems/msbuild/vcpkg.props`와 `vcpkg.targets`도 필요합니다. 실행 파일만 복사한 폴더는 사용할 수 없습니다.

빌드 도중 다운로드 오류가 나면 Visual Studio 출력 창의 **첫 번째 vcpkg 오류**를 확인합니다. 네트워크 연결, 프록시, 디스크 공간, 설치된 C++ 도구를 확인한 뒤 다시 빌드합니다.

`BUILD_FAILED`와 `MSB3073`가 함께 표시되면 라이브러리 설치 실패와 외부 명령 실패를 각각 표시한 것일 수 있습니다. 오류 목록의 마지막 메시지 대신 **출력 → 빌드**에서 먼저 실패한 단계와 종료 코드를 확인합니다. 현재 `AssimpDependencyDiagnostics`를 활성화해 vcpkg의 `--debug` 출력을 기록하도록 설정했습니다. 원인 확인이 끝나면 이 속성을 `false`로 설정해 상세 출력을 끌 수 있습니다.

다음 빌드 후 확인할 로그는 `Client/x64/Release/Client.log` 또는 `Client/x64/Debug/Client.log`입니다. 패키지별 로그는 빌드 출력에 표시된 의존성 작업 폴더의 `buildtrees/<패키지 이름>/`에 있습니다. 예를 들어 zlib의 출력 로그는 `buildtrees/zlib/stdout-x64-windows.log`입니다. 설치 캐시나 다운로드 파일을 지우기 전에 실패 로그를 먼저 확인합니다.

## 실행 파일 전달

다른 PC에서 빌드된 게임만 실행할 때는 Release 실행 파일, 함께 복사된 DLL, `Shaders`, `assets`를 함께 전달합니다. 해당 PC에는 호환되는 Microsoft Visual C++ 런타임도 필요합니다. 의존성의 배포 고지는 의존성 작업 폴더의 `installed/x64-windows/share` 아래 각 패키지의 `copyright` 파일에서 확인할 수 있습니다.

## 모델 처리 범위

`ResourceManager`가 `AssimpModelLoader`를 호출해 CPU 데이터인 `ModelData`를 만들고, 기존 `Model`과 `GRenderer`가 GPU 업로드와 장면 배치를 담당합니다. Assimp 객체는 로딩 중에만 사용하며 장면에 포인터를 남기지 않습니다.

- 대상은 `Meshy_AI_01_Arc_Sentinel_0918131203_texture.fbx`입니다. 원본 FBX 파일을 그대로 사용합니다.
- 노드 계층과 피벗을 보존하며 삼각형 메시, 기본 색상·법선·거칠기·금속성·발광 텍스처를 읽습니다.
- 내장 PNG/JPEG는 WIC로 메모리에서 읽고, 비압축 내장 이미지는 RGBA8로 복사합니다. 임시 이미지 추출 폴더를 만들지 않습니다.
- Meshy의 `texture_0_roughness.png`가 광택 슬롯에 저장된 경우 거칠기 맵으로 해석합니다. 거칠기·금속성 맵이 있으면 원본의 광택·반사 상수로 다시 감쇠하지 않습니다.
- 현재 원본의 +Y 법선맵은 UV의 V 반전에 맞춰 녹색 채널을 한 번 보정합니다. 다른 규약의 법선맵은 별도 로딩 옵션이 필요합니다.
- 좌수계와 행 벡터 행렬로 변환하고 기존 모델의 정면 방향을 유지하도록 루트 방향을 보정합니다. 기존 `Model`의 높이 2m 및 발 위치 보정은 유지합니다.
- 현재는 정적 모델만 표시합니다. 애니메이션·스키닝은 적용하지 않으며 여러 UV 채널이나 텍스처 변환을 한 재질에서 혼합하는 경우 오류를 표시합니다.

이 전환 작업에서는 라이브러리 설치, 빌드, 셰이더 컴파일, 게임 실행을 수행하지 않았습니다. 빌드와 화면 확인은 사용자가 진행합니다.

참고: [Microsoft의 vcpkg MSBuild 연동 안내](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/msbuild-integration), [고정한 vcpkg 기준 버전](https://github.com/microsoft/vcpkg/blob/e03dc9b29710050cd1018bc5674688108658d327/versions/baseline.json), [Assimp 공식 저장소](https://github.com/assimp/assimp).
