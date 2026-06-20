#pragma once
#include "LoadM3d.h"
#include "SkinnedData.h"

#include <assimp/scene.h>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Assimp로 FBX를 읽고, 기존 M3D 스키닝 코드가 쓰는 자료구조로 변환하는 로더이다.
class AssimpLoader
{
public:
	// FBX 메시/재질/본/애니메이션을 읽어서 M3DLoader와 같은 형태로 넘겨준다.
	// Capoeira처럼 정점이 65536개를 넘는 FBX를 위해 인덱스는 32비트로 받는다.
	bool LoadFbx(
		const std::string& filename,
		std::vector<M3DLoader::SkinnedVertex>& vertices,
		std::vector<std::uint32_t>& indices,
		std::vector<M3DLoader::Subset>& subsets,
		std::vector<M3DLoader::M3dMaterial>& mats,
		SkinnedData& skinInfo);

	// 로딩 실패 시 MessageBox로 보여 줄 자세한 실패 원인을 보관한다.
	const std::string& GetLastError() const { return mLastError; }

private:
	struct PendingBoneWeight
	{
		// FBX의 bone weight는 bone을 모두 만든 뒤 vertex에 적용해야 하므로 임시로 모아 둔다.
		UINT VertexIndex = 0;
		std::string BoneName;
		float Weight = 0.0f;
	};

	struct LocalTransform
	{
		// 애니메이션 키가 없는 bone은 bind pose 값을 그대로 사용하기 위해 로컬 변환을 저장한다.
		DirectX::XMFLOAT3 Translation = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 RotationQuat = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	// FBX 노드를 재귀적으로 돌며 mesh를 기존 vertex/index/subset 구조로 변환한다.
	bool ProcessNode(const aiNode* node, const aiScene* scene);
	bool ProcessMesh(const aiMesh* mesh, const aiScene* scene);

	// FBX bone 정보를 읽고, vertex weight는 나중에 적용할 수 있게 임시 배열에 모은다.
	void ReadBones(const aiMesh* mesh, UINT baseVertex);

	// weight가 달린 bone만이 아니라 그 부모 노드까지 보존해 skeleton 계층이 끊기지 않게 한다.
	bool ContainsWeightedBone(const aiNode* node) const;
	void BuildBoneHierarchy(const aiNode* node, int parentBoneIndex);
	void AddMissingBones();
	void ApplyPendingBoneWeights();

	// FBX animation channel을 SkinnedData의 AnimationClip/BoneAnimation 형식으로 변환한다.
	void ReadAnimations(const aiScene* scene);

	// Assimp material을 샘플 프로젝트의 Material 생성 코드가 이해하는 M3dMaterial로 바꾼다.
	M3DLoader::M3dMaterial ConvertMaterial(const aiScene* scene, UINT materialIndex, UINT subsetIndex) const;

	// bone 이름을 index로 바꾸고, bind pose와 offset matrix를 조회하는 보조 함수들이다.
	int CreateBone(const std::string& boneName, int parentBoneIndex);
	int FindBoneIndex(const std::string& boneName) const;
	LocalTransform GetBindPoseTransform(const std::string& boneName) const;
	BoneAnimation BuildStaticBoneAnimation(const std::string& boneName, float endTime) const;

	// Assimp 타입을 DirectXMath 타입으로 바꾸는 변환 함수들이다.
	static DirectX::XMFLOAT2 ToFloat2(const aiVector3D& value);
	static DirectX::XMFLOAT3 ToFloat3(const aiVector3D& value);
	static DirectX::XMFLOAT4 ToFloat4(const aiQuaternion& value);
	static DirectX::XMFLOAT4X4 ToFloat4x4(const aiMatrix4x4& value);
	static std::string ExtractFilename(const std::string& path);
	static bool IsDdsTexture(const std::string& filename);

	// animation key 시간은 FBX tick 단위이므로 초 단위로 바꾼 뒤 보간한다.
	DirectX::XMFLOAT3 SampleVectorKey(
		const aiVectorKey* keys,
		UINT keyCount,
		double time,
		double ticksPerSecond,
		const DirectX::XMFLOAT3& defaultValue) const;
	DirectX::XMFLOAT4 SampleRotationKey(
		const aiQuatKey* keys,
		UINT keyCount,
		double time,
		double ticksPerSecond,
		const DirectX::XMFLOAT4& defaultValue) const;
	double ToSeconds(double ticks, double ticksPerSecond) const;

	// Assimp가 읽은 mesh/bone/animation 정보를 출력 배열에 채우기 위해 임시 포인터로 보관한다.
	std::vector<M3DLoader::SkinnedVertex>* mVertices = nullptr;
	std::vector<std::uint32_t>* mIndices = nullptr;
	std::vector<M3DLoader::Subset>* mSubsets = nullptr;
	std::vector<M3DLoader::M3dMaterial>* mMats = nullptr;
	std::string mModelDirectory = ".";

	// FBX의 bone 이름을 최종 bone index로 바꾸기 위한 테이블과 skeleton 데이터이다.
	std::unordered_map<std::string, int> mBoneNameToIndex;
	std::unordered_map<std::string, DirectX::XMFLOAT4X4> mBoneOffsetByName;
	std::unordered_map<std::string, LocalTransform> mBoneBindPoseByName;
	std::vector<std::string> mBoneNames;
	std::vector<DirectX::XMFLOAT4X4> mBoneOffsets;
	std::vector<int> mBoneHierarchy;
	std::unordered_map<std::string, AnimationClip> mAnimations;

	// 정점마다 최대 4개의 bone weight를 모은 뒤 마지막에 M3D vertex 형식으로 압축한다.
	std::vector<std::array<float, 4>> mVertexWeights;
	std::vector<std::array<BYTE, 4>> mVertexBoneIndices;
	std::vector<PendingBoneWeight> mPendingWeights;

	// LoadFbx 실패 원인을 호출자에게 알려 주기 위한 문자열이다.
	std::string mLastError;
};
