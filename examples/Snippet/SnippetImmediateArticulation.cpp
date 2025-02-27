//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Copyright (c) 2008-2019 NVIDIA Corporation. All rights reserved.
// Copyright (c) 2004-2008 AGEIA Technologies, Inc. All rights reserved.
// Copyright (c) 2001-2004 NovodeX AG. All rights reserved.  

// ****************************************************************************
// This snippet demonstrates the use of immediate articulations.
// ****************************************************************************

#include "PxPhysicsAPI.h"
#include "PxImmediateMode.h"
#include "PsArray.h"
#include "PsHashSet.h"
#include "PsHashMap.h"
#include "ExtConstraintHelper.h"
#include "Bullet3Common/b3Logging.h"
#include "snippetutils/SnippetUtils.h"
#include "snippetcommon/SnippetPrint.h"
#include "snippetcommon/SnippetPVD.h"
#include "PxProfileZone.h"
#define USE_TGS 1

//#define PRINT_TIMINGS
#define TEST_IMMEDIATE_JOINTS

//Enables whether we want persistent state caching (contact cache, friction caching) or not. Avoiding persistency results in one-shot collision detection and zero friction 
//correlation but simplifies code by not longer needing to cache persistent pairs.
#define WITH_PERSISTENCY  1

//Toggles whether we batch constraints or not. Constraint batching is an optional process which can improve performance by grouping together independent constraints. These independent constraints
//can be solved in parallel by using multiple lanes of SIMD registers.
#define BATCH_CONTACTS 1

using namespace physx;
using namespace shdfnd;
using namespace immediate;

static const PxVec3	gGravity(0.0f, -9.81f, 0.0f);
static const float	gContactDistance			= 0.1f;
static const float	gMeshContactMargin			= 0.01f;
static const float	gToleranceLength			= 1.0f;
static const float	gBounceThreshold			= -2.0f;
static const float	gFrictionOffsetThreshold	= 0.04f;
static const float	gCorrelationDistance		= 0.025f;
static const float	gBoundsInflation			= 0.02f;
static const float	gStaticFriction				= 0.5f;
static const float	gDynamicFriction			= 0.5f;
static const float	gRestitution				= 0.0f;
static const float	gMaxDepenetrationVelocity	= 10.0f;
static const float	gMaxContactImpulse			= FLT_MAX;
static const float	gLinearDamping				= 0.1f;
static const float	gAngularDamping				= 0.05f;
static const float	gMaxLinearVelocity			= 100.0f;
static const float	gMaxAngularVelocity			= 100.0f;
static const float	gJointFrictionCoefficient	= 0.05f;
static const PxU32	gNbIterPos					= 4;
static const PxU32	gNbIterVel					= 1;
static bool printContacts = false;
static bool			gPause						= false;
static bool			gOneFrame					= false;
static bool			gDrawBounds					= true;
static PxU32		gSceneIndex					= 0;

#if WITH_PERSISTENCY
struct PersistentContactPair
{
	PersistentContactPair()
	{
		reset();
	}

	PxCache	cache;
	PxU8*	frictions;
	PxU32	nbFrictions;

	PX_FORCE_INLINE	void	reset()
	{
		cache = PxCache();
		frictions = NULL;
		nbFrictions = 0;
	}
};
#endif

	class BlockBasedAllocator
	{
		struct AllocationPage
		{
			static const PxU32 PageSize = 32 * 1024;
			PxU8 mPage[PageSize];

			PxU32 currentIndex;

			AllocationPage() : currentIndex(0){}

			PxU8* allocate(const PxU32 size)
			{
				PxU32 alignedSize = (size + 15)&(~15);
				if ((currentIndex + alignedSize) < PageSize)
				{
					PxU8* ret = &mPage[currentIndex];
					currentIndex += alignedSize;
					return ret;
				}
				return NULL;
			}
		};

		AllocationPage* currentPage;

		Array<AllocationPage*> mAllocatedBlocks;
		PxU32 mCurrentIndex;

	public:
		BlockBasedAllocator() : currentPage(NULL), mCurrentIndex(0)
		{
		}

		virtual PxU8* allocate(const PxU32 byteSize)
		{
			if (currentPage)
			{
				PxU8* data = currentPage->allocate(byteSize);
				if (data)
					return data;
			}

			if (mCurrentIndex < mAllocatedBlocks.size())
			{
				currentPage = mAllocatedBlocks[mCurrentIndex++];
				currentPage->currentIndex = 0;
				return currentPage->allocate(byteSize);
			}
			currentPage = PX_PLACEMENT_NEW(PX_ALLOC(sizeof(AllocationPage), PX_DEBUG_EXP("AllocationPage")), AllocationPage)();
			mAllocatedBlocks.pushBack(currentPage);
			mCurrentIndex = mAllocatedBlocks.size();

			return currentPage->allocate(byteSize);
		}

		void release() { for (PxU32 a = 0; a < mAllocatedBlocks.size(); ++a) PX_FREE(mAllocatedBlocks[a]); mAllocatedBlocks.clear(); currentPage = NULL; mCurrentIndex = 0; }

		void reset() { currentPage = NULL; mCurrentIndex = 0; }

		virtual ~BlockBasedAllocator()
		{
			release();
		}
	};

	class TestCacheAllocator : public PxCacheAllocator
	{
		BlockBasedAllocator mAllocator[2];
		PxU32 currIdx;

	public:

		TestCacheAllocator() : currIdx(0)
		{
		}

		virtual PxU8* allocateCacheData(const PxU32 byteSize)
		{
			return mAllocator[currIdx].allocate(byteSize);
		}

		void release() { currIdx = 1 - currIdx; mAllocator[currIdx].release(); }

		void reset() { currIdx = 1 - currIdx; mAllocator[currIdx].reset(); }

		virtual ~TestCacheAllocator(){}
	};

	class TestConstraintAllocator : public PxConstraintAllocator
	{
		BlockBasedAllocator mConstraintAllocator;
		BlockBasedAllocator mFrictionAllocator[2];

		PxU32 currIdx;
	public:

		TestConstraintAllocator() : currIdx(0)
		{
		}
		virtual PxU8* reserveConstraintData(const PxU32 byteSize){ return mConstraintAllocator.allocate(byteSize); }
		virtual PxU8* reserveFrictionData(const PxU32 byteSize){ return mFrictionAllocator[currIdx].allocate(byteSize); }

		void release() { currIdx = 1 - currIdx; mConstraintAllocator.release(); mFrictionAllocator[currIdx].release(); }

		virtual ~TestConstraintAllocator() {}
	};

	struct IDS
	{
		PX_FORCE_INLINE	IDS(PxU32 id0, PxU32 id1) : mID0(id0), mID1(id1)	{}
		PxU32	mID0;
		PxU32	mID1;

		PX_FORCE_INLINE	bool operator == (const IDS& other) const
		{
			return mID0 == other.mID0 && mID1 == other.mID1;
		}
	};

	PX_FORCE_INLINE uint32_t hash(const IDS& p)
	{
		return shdfnd::hash(uint64_t(p.mID0)|(uint64_t(p.mID1)<<32));
	}

struct MassProps
{
	PxVec3	mInvInertia;
	float	mInvMass;
};

static void computeMassProps(MassProps& props, const PxGeometry& geometry, float mass)
{
	if(mass!=0.0f)
	{
		PxMassProperties inertia(geometry);
		inertia = inertia * (mass/inertia.mass);

		PxQuat orient;
		const PxVec3 diagInertia = PxMassProperties::getMassSpaceInertia(inertia.inertiaTensor, orient);
		props.mInvMass = 1.0f/inertia.mass;
		props.mInvInertia.x = diagInertia.x == 0.0f ? 0.0f : 1.0f/diagInertia.x;
		props.mInvInertia.y = diagInertia.y == 0.0f ? 0.0f : 1.0f/diagInertia.y;
		props.mInvInertia.z = diagInertia.z == 0.0f ? 0.0f : 1.0f/diagInertia.z;
	}
	else
	{
		props.mInvMass = 0.0f;
		props.mInvInertia = PxVec3(0.0f);
	}
}

#ifdef TEST_IMMEDIATE_JOINTS
	struct MyJointData : Ext::JointData
	{
		PxU32		mActors[2];
		PxTransform	mLocalFrames[2];

		void		initInvMassScale()
		{
			invMassScale.linear0	= 1.0f;
			invMassScale.angular0	= 1.0f;
			invMassScale.linear1	= 1.0f;
			invMassScale.angular1	= 1.0f;
		}
	};
#endif

	class ImmediateScene
	{
												PX_NOCOPY(ImmediateScene)
		public:
												ImmediateScene();
												~ImmediateScene();

			void								reset();

			PxU32								createActor(const PxGeometry& geometry, const PxTransform& pose, const MassProps* massProps=NULL, Dy::ArticulationLinkHandle link=0);

			void								createGroundPlane()
												{
													createActor(PxPlaneGeometry(), PxTransformFromPlaneEquation(PxPlane(0.0f, 1.0f, 0.0f, 0.0f)));
												}

			void								createScene();
#ifdef TEST_IMMEDIATE_JOINTS
			void								createSphericalJoint(PxU32 id0, PxU32 id1, const PxTransform& localFrame0, const PxTransform& localFrame1, const PxTransform* pose0=NULL, const PxTransform* pose1=NULL);
#endif
			void								updateArticulations(float dt);
			void								updateBounds();
			void								broadPhase();
			void								narrowPhase();
			void								buildSolverBodyData(float dt);
			void								buildSolverConstraintDesc();
			void								createContactConstraints(float dt, float invDt, float lengthScale, PxU32 nbPositionIterations);
			void								solveAndIntegrate(float dt);

			TestCacheAllocator*					mCacheAllocator;
			TestConstraintAllocator*			mConstraintAllocator;

			// PT: TODO: revisit this basic design once everything works
			Array<PxGeometryHolder>				mGeoms;
			Array<PxTransform>					mPoses;
			Array<PxBounds3>					mBounds;

			class ImmediateActor
			{
				public:
					ImmediateActor()	{}
					~ImmediateActor()	{}

					enum Type
					{
						eSTATIC,
						eDYNAMIC,
						eLINK,
					};
					Type						mType;
					PxU32						mCollisionGroup;
					MassProps					mMassProps;
					PxVec3						mLinearVelocity;
					PxVec3						mAngularVelocity;
					Dy::ArticulationLinkHandle	mLink;
			};
			Array<ImmediateActor>				mActors;
#if USE_TGS
			Array<PxTGSSolverBodyData>			mSolverBodyData;
			Array<PxTGSSolverBodyVel>			mSolverBodies;
			Array<PxTGSSolverBodyTxInertia>		mSolverBodyTxInertias;
#else
			Array<PxSolverBodyData>				mSolverBodyData;
			Array<PxSolverBody>					mSolverBodies;
#endif
			Array<Dy::ArticulationV*>			mArticulations;
#ifdef TEST_IMMEDIATE_JOINTS
			Array<MyJointData>					mJointData;
#endif
			Array<IDS>							mBroadphasePairs;
			HashSet<IDS>						mFilteredPairs;

			struct ContactPair
			{
				PxU32	mID0;
				PxU32	mID1;

				PxU32	mNbContacts;
				PxU32	mStartContactIndex;
			};
			// PT: we use separate arrays here because the immediate mode API expects an array of Gu::ContactPoint
			Array<ContactPair>					mContactPairs;
			Array<Gu::ContactPoint>				mContactPoints;
#if WITH_PERSISTENCY
			HashMap<IDS, PersistentContactPair>	mPersistentPairs;
#endif
			Array<PxSolverConstraintDesc>		mSolverConstraintDesc;
#if BATCH_CONTACTS
			Array<PxSolverConstraintDesc>		mOrderedSolverConstraintDesc;
#endif
			Array<PxConstraintBatchHeader>		mHeaders;
			Array<PxReal>						mContactForces;

			Array<PxVec3>						mMotionLinearVelocity;	// Persistent to avoid runtime allocations but could be managed on the stack
			Array<PxVec3>						mMotionAngularVelocity;	// Persistent to avoid runtime allocations but could be managed on the stack

			PxU32								mNbStaticActors;
			PxU32								mNbArticulationLinks;

			PX_FORCE_INLINE	void	disableCollision(PxU32 i, PxU32 j)
			{
				if(i>j)
					swap(i, j);
				mFilteredPairs.insert(IDS(i, j));
			}

			PX_FORCE_INLINE	bool	isCollisionDisabled(PxU32 i, PxU32 j)	const
			{
				if(i>j)
					swap(i, j);
				return mFilteredPairs.contains(IDS(i, j));
			}

			Dy::ArticulationLinkHandle			mMotorLink;
	};

ImmediateScene::ImmediateScene() :
	mNbStaticActors		(0),
	mNbArticulationLinks(0),
	mMotorLink			(0)
{
	mCacheAllocator = new TestCacheAllocator;
	mConstraintAllocator = new TestConstraintAllocator;
}

ImmediateScene::~ImmediateScene()
{
	reset();

	PX_DELETE_AND_RESET(mConstraintAllocator);
	PX_DELETE_AND_RESET(mCacheAllocator);
}

void ImmediateScene::reset()
{
	mGeoms.clear();
	mPoses.clear();
	mBounds.clear();
	mActors.clear();
	mSolverBodyData.clear();
	mSolverBodies.clear();
	mBroadphasePairs.clear();
	mFilteredPairs.clear();
	mContactPairs.clear();
	mContactPoints.clear();
	mSolverConstraintDesc.clear();
#if BATCH_CONTACTS
	mOrderedSolverConstraintDesc.clear();
#endif
	mHeaders.clear();
	mContactForces.clear();
	mMotionLinearVelocity.clear();
	mMotionAngularVelocity.clear();

	const PxU32 size = mArticulations.size();
	for(PxU32 i=0;i<size;i++)	
		PxReleaseArticulation(mArticulations[i]);

	mArticulations.clear();
#ifdef TEST_IMMEDIATE_JOINTS
	mJointData.clear();
#endif
#if WITH_PERSISTENCY
	mPersistentPairs.clear();
#endif
	mNbStaticActors = mNbArticulationLinks = 0;
	mMotorLink = 0;
}

PxU32 ImmediateScene::createActor(const PxGeometry& geometry, const PxTransform& pose, const MassProps* massProps, Dy::ArticulationLinkHandle link)
{
	const PxU32 id = mActors.size();
	// PT: we don't support compounds in this simple snippet. 1 actor = 1 shape/geom. 
	PX_ASSERT(mGeoms.size()==id);
	PX_ASSERT(mPoses.size()==id);
	PX_ASSERT(mBounds.size()==id);

	const bool isStaticActor = !massProps;
	if(isStaticActor)
	{
		PX_ASSERT(!link);
		mNbStaticActors++;
	}
	else
	{
		// PT: make sure we don't create dynamic actors after static ones. We could reorganize the array but
		// in this simple snippet we just enforce the order in which actors are created.
		PX_ASSERT(!mNbStaticActors);
		if(link)
			mNbArticulationLinks++;
	}

	ImmediateActor actor;
	if(isStaticActor)
		actor.mType			= ImmediateActor::eSTATIC;
	else if(link)
		actor.mType			= ImmediateActor::eLINK;
	else
		actor.mType			= ImmediateActor::eDYNAMIC;
	actor.mCollisionGroup	= 0;
	actor.mLinearVelocity	= PxVec3(0.0f);
	actor.mAngularVelocity	= PxVec3(0.0f);
	actor.mLink				= link;

	if(massProps)
		actor.mMassProps	= *massProps;
	else	
	{
		actor.mMassProps.mInvMass = 0.0f;
		actor.mMassProps.mInvInertia = PxVec3(0.0f);
	}

	mActors.pushBack(actor);

#if USE_TGS
	mSolverBodyData.pushBack(PxTGSSolverBodyData());
	mSolverBodies.pushBack(PxTGSSolverBodyVel());
	mSolverBodyTxInertias.pushBack(PxTGSSolverBodyTxInertia());
#else
	mSolverBodyData.pushBack(PxSolverBodyData());
	mSolverBodies.pushBack(PxSolverBody());
#endif

	mGeoms.pushBack(geometry);
	mPoses.pushBack(pose);
	mBounds.pushBack(PxBounds3());

	return id;
}

static Dy::ArticulationV* createImmediateArticulation(bool fixBase, Array<Dy::ArticulationV*>& articulations)
{
	PxFeatherstoneArticulationData data;
	data.flags = fixBase ? PxArticulationFlag::eFIX_BASE : PxArticulationFlag::Enum(0);
	Dy::ArticulationV* immArt = PxCreateFeatherstoneArticulation(data);
	articulations.pushBack(immArt);
	return immArt;
}

static void setupCommonLinkData(PxFeatherstoneArticulationLinkData& data, Dy::ArticulationLinkHandle parent, const PxTransform& pose, const MassProps& massProps)
{
	data.parent								= parent;
	data.pose								= pose;
	data.inverseMass						= massProps.mInvMass;
	data.inverseInertia						= massProps.mInvInertia;
	data.linearDamping						= gLinearDamping;
	data.angularDamping						= gAngularDamping;
	data.maxLinearVelocitySq				= gMaxLinearVelocity * gMaxLinearVelocity;
	data.maxAngularVelocitySq				= gMaxAngularVelocity * gMaxAngularVelocity;
	data.inboundJoint.frictionCoefficient	= gJointFrictionCoefficient;
}

#ifdef TEST_IMMEDIATE_JOINTS
void ImmediateScene::createSphericalJoint(PxU32 id0, PxU32 id1, const PxTransform& localFrame0, const PxTransform& localFrame1, const PxTransform* pose0, const PxTransform* pose1)
{
	const bool isStatic0 = mActors[id0].mType == ImmediateActor::eSTATIC;
	const bool isStatic1 = mActors[id1].mType == ImmediateActor::eSTATIC;

	MyJointData jointData;
	jointData.mActors[0]		= id0;
	jointData.mActors[1]		= id1;
	jointData.mLocalFrames[0]	= localFrame0;
	jointData.mLocalFrames[1]	= localFrame1;
	if(isStatic0)
		jointData.c2b[0]		= pose0->getInverse().transformInv(localFrame0);
	else
		jointData.c2b[0]		= localFrame0;
	if(isStatic1)
		jointData.c2b[1]		= pose1->getInverse().transformInv(localFrame1);
	else
		jointData.c2b[1]		= localFrame1;

	jointData.initInvMassScale();

	mJointData.pushBack(jointData);
	disableCollision(id0, id1);
}
#endif

void ImmediateScene::createScene()
{
	mMotorLink = 0;

	const PxU32 index = gSceneIndex;
	if(index==0)
	{
		// Box stack
		{
			const PxVec3 extents(0.5f, 0.5f, 0.5f);
			const PxBoxGeometry boxGeom(extents);
			//const PxSphereGeometry boxGeom(0.5);// extents);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

	//		for(PxU32 i=0;i<8;i++)
	//			createBox(extents, PxTransform(PxVec3(0.0f, extents.y + float(i)*extents.y*2.0f, 0.0f)), 1.0f);

			PxU32 size = 28;
//			PxU32 size = 2;
//			PxU32 size = 1;
			float y = extents.y;
			float x = 0.0f;
			while(size)
			{
				for(PxU32 i=0;i<1;i++)
					createActor(boxGeom, PxTransform(PxVec3(x+float(i)*extents.x*2.0f, y, 0.0f)), &massProps);

				//x += extents.x;
				y += extents.y*2.0f;
				size--;
			}
		}

		createGroundPlane();
	}
	else if(index==1)
	{
		// Simple scene with regular spherical joint

#ifdef TEST_IMMEDIATE_JOINTS
		const float boxSize = 1.0f;
		const PxVec3 extents(boxSize, boxSize, boxSize);
		const PxBoxGeometry boxGeom(extents);

		MassProps massProps;
		computeMassProps(massProps, boxGeom, 1.0f);

		const PxVec3 staticPos(0.0f, 6.0f, 0.0f);
		const PxVec3 dynamicPos = staticPos - extents*2.0f;

		const PxTransform dynPose(dynamicPos);
		const PxTransform staticPose(staticPos);

		const PxU32 dynamicObject = createActor(boxGeom, dynPose, &massProps);
		const PxU32 staticObject = createActor(boxGeom, staticPose);

		createSphericalJoint(staticObject, dynamicObject, PxTransform(-extents), PxTransform(extents), &staticPose, &dynPose);
#endif
	}
	else if(index==2)
	{
		// RC articulation with contacts

		if(1)
		{
			const PxBoxGeometry boxGeom(PxVec3(1.0f));

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 0.5f);

			createActor(boxGeom, PxTransform(PxVec3(0.0f, 1.0f, 0.0f)), &massProps);
		}

		const PxU32 nbLinks = 6;
		const float Altitude = 6.0f;
		const PxTransform basePose(PxVec3(0.f, Altitude, 0.f));
		const PxVec3 boxExtents(0.5f, 0.1f, 0.5f);
		const PxBoxGeometry boxGeom(boxExtents);
		const float s = boxExtents.x*1.1f;

		MassProps massProps;
		computeMassProps(massProps, boxGeom, 1.0f);

		Dy::ArticulationV* immArt = createImmediateArticulation(true, mArticulations);

		Dy::ArticulationLinkHandle base;
		PxU32 baseID;
		{
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, 0, basePose, massProps);
			base = PxAddArticulationLink(immArt, linkData);
			baseID = createActor(boxGeom, basePose, &massProps, base);
		}

		Dy::ArticulationLinkHandle parent = base;
		PxU32 parentID = baseID;
		PxTransform linkPose = basePose;
		for(PxU32 i=0;i<nbLinks;i++)
		{
			linkPose.p.z += s*2.0f;

			Dy::ArticulationLinkHandle link;
			PxU32 linkID;
			{
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, parent, linkPose, massProps);
				//
				linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.0f, 0.0f, s));
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.0f, 0.0f, -s));
				linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eFREE;

				link = PxAddArticulationLink(immArt, linkData, i==nbLinks-1);
				linkID = createActor(boxGeom, linkPose, &massProps, link);

				disableCollision(parentID, linkID);
			}

			parent = link;
			parentID = linkID;
		}

		createGroundPlane();
	}
	else if(index==3)
	{
		// RC articulation with limits

		const PxU32 nbLinks = 4;
		const float Altitude = 6.0f;
		const PxTransform basePose(PxVec3(0.f, Altitude, 0.f));
		const PxVec3 boxExtents(0.5f, 0.1f, 0.5f);
		const PxBoxGeometry boxGeom(boxExtents);
		const float s = boxExtents.x*1.1f;

		MassProps massProps;
		computeMassProps(massProps, boxGeom, 1.0f);

		Dy::ArticulationV* immArt = createImmediateArticulation(true, mArticulations);

		Dy::ArticulationLinkHandle base;
		PxU32 baseID;
		{
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, 0, basePose, massProps);
			base = PxAddArticulationLink(immArt, linkData);
			baseID = createActor(boxGeom, basePose, &massProps, base);
		}

		Dy::ArticulationLinkHandle parent = base;
		PxU32 parentID = baseID;
		PxTransform linkPose = basePose;
		for(PxU32 i=0;i<nbLinks;i++)
		{
			linkPose.p.z += s*2.0f;

			Dy::ArticulationLinkHandle link;
			PxU32 linkID;
			{
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, parent, linkPose, massProps);
				//
				linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.0f, 0.0f, s));
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.0f, 0.0f, -s));
				linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eLIMITED;
				linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].low = -PxPi/8.0f;
				linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].high = PxPi/8.0f;

				link = PxAddArticulationLink(immArt, linkData, i==nbLinks-1);
				linkID = createActor(boxGeom, linkPose, &massProps, link);

				disableCollision(parentID, linkID);
			}

			parent = link;
			parentID = linkID;
		}
	}
	else if(index==4)
	{
		if(0)
		{
			const float Altitude = 6.0f;
			const PxTransform basePose(PxVec3(0.f, Altitude, 0.f));
			const PxVec3 boxExtents(0.5f, 0.1f, 0.5f);
			const PxBoxGeometry boxGeom(boxExtents);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

			Dy::ArticulationV* immArt = createImmediateArticulation(false, mArticulations);

			Dy::ArticulationLinkHandle base;
			PxU32 baseID;
			{
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, 0, basePose, massProps);

				base = PxAddArticulationLink(immArt, linkData, true);
				baseID = createActor(boxGeom, basePose, &massProps, base);
				PX_UNUSED(baseID);
			}

			return;
		}


		// RC articulation with drive

		const PxU32 nbLinks = 1;
		const float Altitude = 6.0f;
		const PxTransform basePose(PxVec3(0.f, Altitude, 0.f));
		const PxVec3 boxExtents(0.5f, 0.1f, 0.5f);
		const PxBoxGeometry boxGeom(boxExtents);
		const float s = boxExtents.x*1.1f;

		MassProps massProps;
		computeMassProps(massProps, boxGeom, 1.0f);

		Dy::ArticulationV* immArt = createImmediateArticulation(true, mArticulations);

		Dy::ArticulationLinkHandle base;
		PxU32 baseID;
		{
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, 0, basePose, massProps);
			base = PxAddArticulationLink(immArt, linkData);
			baseID = createActor(boxGeom, basePose, &massProps, base);
		}

		Dy::ArticulationLinkHandle parent = base;
		PxU32 parentID = baseID;
		PxTransform linkPose = basePose;
		for(PxU32 i=0;i<nbLinks;i++)
		{
			linkPose.p.z += s*2.0f;

			Dy::ArticulationLinkHandle link;
			PxU32 linkID;
			{
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, parent, linkPose, massProps);
				//
				linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.0f, 0.0f, s));
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.0f, 0.0f, -s));
				linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eFREE;
				linkData.inboundJoint.drives[PxArticulationAxis::eTWIST].stiffness = 0.0f;
				linkData.inboundJoint.drives[PxArticulationAxis::eTWIST].damping = 1000.0f;
				linkData.inboundJoint.drives[PxArticulationAxis::eTWIST].maxForce = FLT_MAX;
				linkData.inboundJoint.drives[PxArticulationAxis::eTWIST].driveType = PxArticulationDriveType::eFORCE;
				linkData.inboundJoint.targetVel[PxArticulationAxis::eTWIST] = 4.0f;

				link = PxAddArticulationLink(immArt, linkData, i==nbLinks-1);
				linkID = createActor(boxGeom, linkPose, &massProps, link);

				disableCollision(parentID, linkID);

				mMotorLink = link;
			}

			parent = link;
			parentID = linkID;
		}
	}
	else if(index==5)
	{
		// Scissor lift
		const PxReal runnerLength = 2.f;
		const PxReal placementDistance = 1.8f;

		const PxReal cosAng = (placementDistance) / (runnerLength);

		const PxReal angle = PxAcos(cosAng);
		const PxReal sinAng = PxSin(angle);

		const PxQuat leftRot(-angle, PxVec3(1.f, 0.f, 0.f));
		const PxQuat rightRot(angle, PxVec3(1.f, 0.f, 0.f));

		Dy::ArticulationV* immArt = createImmediateArticulation(false, mArticulations);

		//

		Dy::ArticulationLinkHandle base;
		PxU32 baseID;
		{
			const PxBoxGeometry boxGeom(0.5f, 0.25f, 1.5f);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 3.0f);

			const PxTransform pose(PxVec3(0.f, 0.25f, 0.f));
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, 0, pose, massProps);
			base = PxAddArticulationLink(immArt, linkData);
			baseID = createActor(boxGeom, pose, &massProps, base);
		}

		//

		Dy::ArticulationLinkHandle leftRoot;
		PxU32 leftRootID;
		{
			const PxBoxGeometry boxGeom(0.5f, 0.05f, 0.05f);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

			const PxTransform pose(PxVec3(0.f, 0.55f, -0.9f));
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, base, pose, massProps);

				linkData.inboundJoint.type			= PxArticulationJointType::eFIX;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.f, 0.25f, -0.9f));
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, -0.05f, 0.f));

			leftRoot = PxAddArticulationLink(immArt, linkData);
			leftRootID = createActor(boxGeom, pose, &massProps, leftRoot);
			disableCollision(baseID, leftRootID);
		}

		//

		Dy::ArticulationLinkHandle rightRoot;
		PxU32 rightRootID;
		{
			const PxBoxGeometry boxGeom(0.5f, 0.05f, 0.05f);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

			const PxTransform pose(PxVec3(0.f, 0.55f, 0.9f));
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, base, pose, massProps);

				linkData.inboundJoint.type			= PxArticulationJointType::ePRISMATIC;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.f, 0.25f, 0.9f));
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, -0.05f, 0.f));
				linkData.inboundJoint.motion[PxArticulationAxis::eZ] = PxArticulationMotion::eLIMITED;
				linkData.inboundJoint.limits[PxArticulationAxis::eZ].low = -1.4f;
				linkData.inboundJoint.limits[PxArticulationAxis::eZ].high = 0.2f;
				if(0)
				{
					linkData.inboundJoint.drives[PxArticulationAxis::eZ].stiffness = 100000.f;
					linkData.inboundJoint.drives[PxArticulationAxis::eZ].damping = 0.f;
					linkData.inboundJoint.drives[PxArticulationAxis::eZ].maxForce = PX_MAX_F32;
					linkData.inboundJoint.drives[PxArticulationAxis::eZ].driveType = PxArticulationDriveType::eFORCE;
				}

			rightRoot = PxAddArticulationLink(immArt, linkData);
			rightRootID = createActor(boxGeom, pose, &massProps, rightRoot);
			disableCollision(baseID, rightRootID);
		}

		//

		const PxU32 linkHeight = 3;
		PxU32 currLeftID = leftRootID;
		PxU32 currRightID = rightRootID;
		Dy::ArticulationLinkHandle currLeft = leftRoot;
		Dy::ArticulationLinkHandle currRight = rightRoot;
		PxQuat rightParentRot(PxIdentity);
		PxQuat leftParentRot(PxIdentity);
		for(PxU32 i=0; i<linkHeight; ++i)
		{
			const PxVec3 pos(0.5f, 0.55f + 0.1f*(1 + i), 0.f);

			Dy::ArticulationLinkHandle leftLink;
			PxU32 leftLinkID;
			{
				const PxBoxGeometry boxGeom(0.05f, 0.05f, 1.f);

				MassProps massProps;
				computeMassProps(massProps, boxGeom, 1.0f);

				const PxTransform pose(pos + PxVec3(0.f, sinAng*(2 * i + 1), 0.f), leftRot);
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, currLeft, pose, massProps);

					const PxVec3 leftAnchorLocation = pos + PxVec3(0.f, sinAng*(2 * i), -0.9f);
					linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
					linkData.inboundJoint.parentPose	= PxTransform(mPoses[currLeftID].transformInv(leftAnchorLocation), leftParentRot);
					linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, 0.f, -1.f), rightRot);
					linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eLIMITED;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].low = -PxPi;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].high = angle;

				leftLink = PxAddArticulationLink(immArt, linkData);
				leftLinkID = createActor(boxGeom, pose, &massProps, leftLink);
				disableCollision(currLeftID, leftLinkID);
				mActors[leftLinkID].mCollisionGroup = 1;
			}
			leftParentRot = leftRot;

			//

			Dy::ArticulationLinkHandle rightLink;
			PxU32 rightLinkID;
			{
				const PxBoxGeometry boxGeom(0.05f, 0.05f, 1.f);

				MassProps massProps;
				computeMassProps(massProps, boxGeom, 1.0f);

				const PxTransform pose(pos + PxVec3(0.f, sinAng*(2 * i + 1), 0.f), rightRot);
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, currRight, pose, massProps);

					const PxVec3 rightAnchorLocation = pos + PxVec3(0.f, sinAng*(2 * i), 0.9f);
					linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
					linkData.inboundJoint.parentPose	= PxTransform(mPoses[currRightID].transformInv(rightAnchorLocation), rightParentRot);
					linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, 0.f, 1.f), leftRot);
					linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eLIMITED;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].low = -angle;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].high = PxPi;

				rightLink = PxAddArticulationLink(immArt, linkData);
				rightLinkID = createActor(boxGeom, pose, &massProps, rightLink);
				disableCollision(currRightID, rightLinkID);
				mActors[rightLinkID].mCollisionGroup = 1;
			}
			rightParentRot = rightRot;

#ifdef TEST_IMMEDIATE_JOINTS
			createSphericalJoint(leftLinkID, rightLinkID, PxTransform(PxIdentity), PxTransform(PxIdentity));
#else
			disableCollision(leftLinkID, rightLinkID);
#endif
			currLeftID = rightLinkID;
			currRightID = leftLinkID;
			currLeft = rightLink;
			currRight = leftLink;
		}

		//

		Dy::ArticulationLinkHandle leftTop;
		PxU32 leftTopID;
		{
			const PxBoxGeometry boxGeom(0.5f, 0.05f, 0.05f);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

			const PxTransform pose(mPoses[currLeftID].transform(PxTransform(PxVec3(-0.5f, 0.f, -1.0f), leftParentRot)));
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, currLeft, pose, massProps);

				linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.f, 0.f, -1.f), mPoses[currLeftID].q.getConjugate());
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.5f, 0.f, 0.f), pose.q.getConjugate());
				linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eFREE;

			leftTop = PxAddArticulationLink(immArt, linkData);
			leftTopID = createActor(boxGeom, pose, &massProps, leftTop);
			disableCollision(currLeftID, leftTopID);
			mActors[leftTopID].mCollisionGroup = 1;
		}

		//

		Dy::ArticulationLinkHandle rightTop;
		PxU32 rightTopID;
		{
			// TODO: use a capsule here
//	PxRigidActorExt::createExclusiveShape(*rightTop, PxCapsuleGeometry(0.05f, 0.8f), *gMaterial);
			const PxBoxGeometry boxGeom(0.5f, 0.05f, 0.05f);

			MassProps massProps;
			computeMassProps(massProps, boxGeom, 1.0f);

			const PxTransform pose(mPoses[currRightID].transform(PxTransform(PxVec3(-0.5f, 0.f, 1.0f), rightParentRot)));
			PxFeatherstoneArticulationLinkData linkData;
			setupCommonLinkData(linkData, currRight, pose, massProps);

				linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
				linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.f, 0.f, 1.f), mPoses[currRightID].q.getConjugate());
				linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.5f, 0.f, 0.f), pose.q.getConjugate());
				linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eFREE;

			rightTop = PxAddArticulationLink(immArt, linkData);
			rightTopID = createActor(boxGeom, pose, &massProps, rightTop);
			disableCollision(currRightID, rightTopID);
			mActors[rightTopID].mCollisionGroup = 1;
		}

		//

		currLeftID = leftRootID;
		currRightID = rightRootID;
		currLeft = leftRoot;
		currRight = rightRoot;
		rightParentRot = PxQuat(PxIdentity);
		leftParentRot = PxQuat(PxIdentity);

		for(PxU32 i=0; i<linkHeight; ++i)
		{
			const PxVec3 pos(-0.5f, 0.55f + 0.1f*(1 + i), 0.f);

			Dy::ArticulationLinkHandle leftLink;
			PxU32 leftLinkID;
			{
				const PxBoxGeometry boxGeom(0.05f, 0.05f, 1.f);

				MassProps massProps;
				computeMassProps(massProps, boxGeom, 1.0f);

				const PxTransform pose(pos + PxVec3(0.f, sinAng*(2 * i + 1), 0.f), leftRot);
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, currLeft, pose, massProps);

					const PxVec3 leftAnchorLocation = pos + PxVec3(0.f, sinAng*(2 * i), -0.9f);
					linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
					linkData.inboundJoint.parentPose	= PxTransform(mPoses[currLeftID].transformInv(leftAnchorLocation), leftParentRot);
					linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, 0.f, -1.f), rightRot);
					linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eLIMITED;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].low = -PxPi;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].high = angle;

				leftLink = PxAddArticulationLink(immArt, linkData);
				leftLinkID = createActor(boxGeom, pose, &massProps, leftLink);
				disableCollision(currLeftID, leftLinkID);
				mActors[leftLinkID].mCollisionGroup = 1;
			}
			leftParentRot = leftRot;

			//

			Dy::ArticulationLinkHandle rightLink;
			PxU32 rightLinkID;
			{
				const PxBoxGeometry boxGeom(0.05f, 0.05f, 1.f);

				MassProps massProps;
				computeMassProps(massProps, boxGeom, 1.0f);

				const PxTransform pose(pos + PxVec3(0.f, sinAng*(2 * i + 1), 0.f), rightRot);
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, currRight, pose, massProps);

					const PxVec3 rightAnchorLocation = pos + PxVec3(0.f, sinAng*(2 * i), 0.9f);
					linkData.inboundJoint.type			= PxArticulationJointType::eREVOLUTE;
					linkData.inboundJoint.parentPose	= PxTransform(mPoses[currRightID].transformInv(rightAnchorLocation), rightParentRot);
					linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, 0.f, 1.f), leftRot);
					linkData.inboundJoint.motion[PxArticulationAxis::eTWIST] = PxArticulationMotion::eLIMITED;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].low = -angle;
					linkData.inboundJoint.limits[PxArticulationAxis::eTWIST].high = PxPi;

				rightLink = PxAddArticulationLink(immArt, linkData);
				rightLinkID = createActor(boxGeom, pose, &massProps, rightLink);
				disableCollision(currRightID, rightLinkID);
				mActors[rightLinkID].mCollisionGroup = 1;
			}
			rightParentRot = rightRot;

#ifdef TEST_IMMEDIATE_JOINTS
			createSphericalJoint(leftLinkID, rightLinkID, PxTransform(PxIdentity), PxTransform(PxIdentity));
#else
			disableCollision(leftLinkID, rightLinkID);
#endif
			currLeftID = rightLinkID;
			currRightID = leftLinkID;
			currLeft = rightLink;
			currRight = leftLink;
		}

		//

#ifdef TEST_IMMEDIATE_JOINTS
		createSphericalJoint(currLeftID, leftTopID, PxTransform(PxVec3(0.f, 0.f, -1.f)), PxTransform(PxVec3(-0.5f, 0.f, 0.f)));
		createSphericalJoint(currRightID, rightTopID, PxTransform(PxVec3(0.f, 0.f, 1.f)), PxTransform(PxVec3(-0.5f, 0.f, 0.f)));
#else
		disableCollision(currLeftID, leftTopID);
		disableCollision(currRightID, rightTopID);
#endif
		//

		// Create top
		{
			Dy::ArticulationLinkHandle top;
			PxU32 topID;
			{
				const PxBoxGeometry boxGeom(0.5f, 0.1f, 1.5f);

				MassProps massProps;
				computeMassProps(massProps, boxGeom, 1.0f);

				const PxTransform pose(PxVec3(0.f, mPoses[leftTopID].p.y + 0.15f, 0.f));
				PxFeatherstoneArticulationLinkData linkData;
				setupCommonLinkData(linkData, leftTop, pose, massProps);

					linkData.inboundJoint.type			= PxArticulationJointType::eFIX;
					linkData.inboundJoint.parentPose	= PxTransform(PxVec3(0.f, 0.0f, 0.f));
					linkData.inboundJoint.childPose		= PxTransform(PxVec3(0.f, -0.15f, -0.9f));

				top = PxAddArticulationLink(immArt, linkData, true);
				topID = createActor(boxGeom, pose, &massProps, top);
				disableCollision(leftTopID, topID);
			}
		}

		//

		createGroundPlane();
	}
}

void ImmediateScene::updateArticulations(float dt)
{
#if USE_TGS
	const float stepDt = dt/gNbIterPos;
	const float invTotalDt = 1.0f/dt;
	const float stepInvDt = 1.0f/stepDt;
#endif
	
	const PxU32 nbArticulations = mArticulations.size();
	for(PxU32 i=0;i<nbArticulations;i++)
	{
		Dy::ArticulationV* articulation = mArticulations[i];
#if USE_TGS
		PxComputeUnconstrainedVelocitiesTGS(articulation, gGravity, stepDt, dt, stepInvDt, invTotalDt);
#else
		PxComputeUnconstrainedVelocities(articulation, gGravity, dt);
#endif
	}
}

void ImmediateScene::updateBounds()
{
	// PT: in this snippet we simply recompute all bounds each frame (i.e. even static ones)
	const PxU32 nbActors = mActors.size();
	for(PxU32 i=0;i<nbActors;i++)
	{
		const PxGeometry& currentGeom = mGeoms[i].any();
		const PxTransform& currentPose = mPoses[i];

		const PxVec3& center = currentPose.p;
		PxVec3 extents;

		switch(currentGeom.getType())
		{
			case PxGeometryType::ePLANE:
			{
				extents = PxVec3(1000000.0f);
			}
			break;
			case PxGeometryType::eBOX:
			{
				const PxBoxGeometry& boxGeom = static_cast<const PxBoxGeometry&>(currentGeom);
				extents = boxGeom.halfExtents;
			}
			break;

			case PxGeometryType::eSPHERE:
			case PxGeometryType::eCAPSULE:
			case PxGeometryType::eCONVEXMESH:
			case PxGeometryType::eTRIANGLEMESH:
			case PxGeometryType::eHEIGHTFIELD:
			case PxGeometryType::eGEOMETRY_COUNT:
			case PxGeometryType::eINVALID:
				PX_ASSERT(0);
			break;
		}

		extents += PxVec3(gBoundsInflation);

		mBounds[i] = PxBounds3::basisExtent(center, PxMat33(currentPose.q), extents);
	}
}

void ImmediateScene::broadPhase()
{
	// PT: in this snippet we simply do a brute-force O(n^2) broadphase between all actors
	mBroadphasePairs.clear();
	const PxU32 nbActors = mActors.size();
	for(PxU32 i=0; i<nbActors; i++)
	{
		const ImmediateActor::Type type0 = mActors[i].mType;

		for(PxU32 j=i+1; j<nbActors; j++)
		{
			const ImmediateActor::Type type1 = mActors[j].mType;

			// Filtering
			{
				if(type0==ImmediateActor::eSTATIC && type1==ImmediateActor::eSTATIC)
					continue;

				if(mActors[i].mCollisionGroup==1 && mActors[j].mCollisionGroup==1)
					continue;

				if(isCollisionDisabled(i, j))
					continue;
			}

			if(mBounds[i].intersects(mBounds[j]))
			{
				mBroadphasePairs.pushBack(IDS(i, j));
			}
#if WITH_PERSISTENCY
			else
			{
				const HashMap<IDS, PersistentContactPair>::Entry* e = mPersistentPairs.find(IDS(i, j));
				if(e)
				{
					PersistentContactPair& persistentData = const_cast<PersistentContactPair&>(e->second);
					//No collision detection performed at all so clear contact cache and friction data
					persistentData.reset();
				}
			}
#endif
		}
	}
}

void ImmediateScene::narrowPhase()
{
	class ContactRecorder : public PxContactRecorder
	{
		public:
						ContactRecorder(ImmediateScene* scene, PxU32 id0, PxU32 id1) : mScene(scene), mID0(id0), mID1(id1), mHasContacts(false)	{}

		virtual	bool	recordContacts(const Gu::ContactPoint* contactPoints, const PxU32 nbContacts, const PxU32 /*index*/)
		{
			{
				ImmediateScene::ContactPair pair;
				pair.mID0				= mID0;
				pair.mID1				= mID1;
				pair.mNbContacts		= nbContacts;
				pair.mStartContactIndex	= mScene->mContactPoints.size();
				mScene->mContactPairs.pushBack(pair);
				mHasContacts = true;
			}

			for(PxU32 i=0; i<nbContacts; i++)
			{
				// Fill in solver-specific data that our contact gen does not produce...
				Gu::ContactPoint point = contactPoints[i];
				point.maxImpulse		= PX_MAX_F32;
				point.targetVel			= PxVec3(0.0f);
				point.staticFriction	= gStaticFriction;
				point.dynamicFriction	= gDynamicFriction;
				point.restitution		= gRestitution;
				point.materialFlags		= 0;
				mScene->mContactPoints.pushBack(point);
			}
			return true;
		}

		ImmediateScene*	mScene;
		PxU32			mID0;
		PxU32			mID1;
		bool			mHasContacts;
	};

	mCacheAllocator->reset();
	mContactPairs.resize(0);
	mContactPoints.resize(0);

	const PxU32 nbPairs = mBroadphasePairs.size();
	for(PxU32 i=0;i<nbPairs;i++)
	{
		const IDS& pair = mBroadphasePairs[i];

		const PxTransform& tr0 = mPoses[pair.mID0];
		const PxTransform& tr1 = mPoses[pair.mID1];

		const PxGeometry* pxGeom0 = &mGeoms[pair.mID0].any();
		const PxGeometry* pxGeom1 = &mGeoms[pair.mID1].any();

		ContactRecorder contactRecorder(this, pair.mID0, pair.mID1);

#if WITH_PERSISTENCY
		PersistentContactPair& persistentData = mPersistentPairs[IDS(pair.mID0, pair.mID1)];

		PxGenerateContacts(&pxGeom0, &pxGeom1, &tr0, &tr1, &persistentData.cache, 1, contactRecorder, gContactDistance, gMeshContactMargin, gToleranceLength, *mCacheAllocator);
		if(!contactRecorder.mHasContacts)
		{
			//Contact generation run but no touches found so clear cached friction data
			persistentData.frictions = NULL;
			persistentData.nbFrictions = 0;
		}
#else
		PxCache cache;
		PxGenerateContacts(&pxGeom0, &pxGeom1, &tr0, &tr1, &cache, 1, contactRecorder, gContactDistance, gMeshContactMargin, gToleranceLength, *mCacheAllocator);
#endif
	}

	if(printContacts)
		printf("Narrow-phase: %d contacts    \n", mContactPoints.size());
}

void ImmediateScene::buildSolverBodyData(float dt)
{
	const PxU32 nbActors = mActors.size();
	for(PxU32 i=0;i<nbActors;i++)
	{
		if(mActors[i].mType==ImmediateActor::eSTATIC)
		{
#if USE_TGS
			PxConstructStaticSolverBodyTGS(mPoses[i], mSolverBodies[i], mSolverBodyTxInertias[i], mSolverBodyData[i]);
#else
			PxConstructStaticSolverBody(mPoses[i], mSolverBodyData[i]);
#endif
		}
		else
		{
			PxRigidBodyData data;
			data.linearVelocity				= mActors[i].mLinearVelocity;
			data.angularVelocity			= mActors[i].mAngularVelocity;
			data.invMass					= mActors[i].mMassProps.mInvMass;
			data.invInertia					= mActors[i].mMassProps.mInvInertia;
			data.body2World					= mPoses[i];
			data.maxDepenetrationVelocity	= gMaxDepenetrationVelocity;
			data.maxContactImpulse			= gMaxContactImpulse;
			data.linearDamping				= gLinearDamping;
			data.angularDamping				= gAngularDamping;
			data.maxLinearVelocitySq		= gMaxLinearVelocity*gMaxLinearVelocity;
			data.maxAngularVelocitySq		= gMaxAngularVelocity*gMaxAngularVelocity;
#if USE_TGS
			PxConstructSolverBodiesTGS(&data, &mSolverBodies[i], &mSolverBodyTxInertias[i], &mSolverBodyData[i], 1, gGravity, dt);
#else
			PxConstructSolverBodies(&data, &mSolverBodyData[i], 1, gGravity, dt);
#endif
		}
	}
}

#if USE_TGS
static void setupDesc(PxSolverConstraintDesc& desc, const ImmediateScene::ImmediateActor* actors, PxTGSSolverBodyVel* solverBodies, PxU32 id, bool aorb)
#else
static void setupDesc(PxSolverConstraintDesc& desc, const ImmediateScene::ImmediateActor* actors, PxSolverBody* solverBodies, PxU32 id, bool aorb)
#endif
{
	if(!aorb)
		desc.bodyADataIndex	= PxU16(id);
	else
		desc.bodyBDataIndex	= PxU16(id);

	Dy::ArticulationLinkHandle link = actors[id].mLink;
	if(link)
	{
		if(!aorb)
		{
			desc.articulationA	= PxGetLinkArticulation(link);
			desc.linkIndexA		= PxU16(PxGetLinkIndex(link));
		}
		else
		{
			desc.articulationB	= PxGetLinkArticulation(link);
			desc.linkIndexB		= PxU16(PxGetLinkIndex(link));
		}
	}
	else
	{
		if(!aorb)
		{
#if USE_TGS
			desc.tgsBodyA		= &solverBodies[id];
#else
			desc.bodyA			= &solverBodies[id];
#endif
			desc.linkIndexA		= PxSolverConstraintDesc::NO_LINK;
		}
		else
		{
#if USE_TGS
			desc.tgsBodyB		= &solverBodies[id];
#else
			desc.bodyB			= &solverBodies[id];
#endif
			desc.linkIndexB		= PxSolverConstraintDesc::NO_LINK;
		}
	}
}

void ImmediateScene::buildSolverConstraintDesc()
{
	const PxU32 nbContactPairs = mContactPairs.size();
#ifdef TEST_IMMEDIATE_JOINTS
	const PxU32 nbJoints = mJointData.size();
	mSolverConstraintDesc.resize(nbContactPairs+nbJoints);
#else
	mSolverConstraintDesc.resize(nbContactPairs);
#endif

	for(PxU32 i=0; i<nbContactPairs; i++)
	{
		const ContactPair& pair = mContactPairs[i];
		PxSolverConstraintDesc& desc = mSolverConstraintDesc[i];

		setupDesc(desc, mActors.begin(), mSolverBodies.begin(), pair.mID0, false);
		setupDesc(desc, mActors.begin(), mSolverBodies.begin(), pair.mID1, true);

		//Cache pointer to our contact data structure and identify which type of constraint this is. We'll need this later after batching.
		//If we choose not to perform batching and instead just create a single header per-pair, then this would not be necessary because
		//the constraintDescs would not have been reordered
		desc.constraint				= reinterpret_cast<PxU8*>(const_cast<ContactPair*>(&pair));
		desc.constraintLengthOver16	= PxSolverConstraintDesc::eCONTACT_CONSTRAINT;
	}

#ifdef TEST_IMMEDIATE_JOINTS
	for(PxU32 i=0; i<nbJoints; i++)
	{
		const MyJointData& jointData = mJointData[i];
		PxSolverConstraintDesc& desc = mSolverConstraintDesc[nbContactPairs+i];

		const PxU32 id0 = jointData.mActors[0];
		const PxU32 id1 = jointData.mActors[1];

		setupDesc(desc, mActors.begin(), mSolverBodies.begin(), id0, false);
		setupDesc(desc, mActors.begin(), mSolverBodies.begin(), id1, true);

		desc.constraint	= reinterpret_cast<PxU8*>(const_cast<MyJointData*>(&jointData));
		desc.constraintLengthOver16 = PxSolverConstraintDesc::eJOINT_CONSTRAINT;
	}
#endif
}

#ifdef TEST_IMMEDIATE_JOINTS

// PT: this is copied from PxExtensions, it's the solver prep function for spherical joints
static PxU32 SphericalJointSolverPrep(Px1DConstraint* constraints,
	PxVec3& body0WorldOffset,
	PxU32 /*maxConstraints*/,
	PxConstraintInvMassScale& invMassScale,
	const void* constantBlock,							  
	const PxTransform& bA2w,
	const PxTransform& bB2w,
	bool /*useExtendedLimits*/,
	PxVec3& cA2wOut, PxVec3& cB2wOut)
{
	const MyJointData& data = *reinterpret_cast<const MyJointData*>(constantBlock);

	PxTransform cA2w, cB2w;
	Ext::joint::ConstraintHelper ch(constraints, invMassScale, cA2w, cB2w, body0WorldOffset, data, bA2w, bB2w);

	if(cB2w.q.dot(cA2w.q)<0.0f)
		cB2w.q = -cB2w.q;

/*	if(data.jointFlags & PxSphericalJointFlag::eLIMIT_ENABLED)
	{
		PxQuat swing, twist;
		Ps::separateSwingTwist(cA2w.q.getConjugate() * cB2w.q, swing, twist);
		PX_ASSERT(PxAbs(swing.x)<1e-6f);

		// PT: TODO: refactor with D6 joint code
		PxVec3 axis;
		PxReal error;
		const PxReal pad = data.limit.isSoft() ? 0.0f : data.limit.contactDistance;
		const Cm::ConeLimitHelperTanLess coneHelper(data.limit.yAngle, data.limit.zAngle, pad);
		const bool active = coneHelper.getLimit(swing, axis, error);				
		if(active)
			ch.angularLimit(cA2w.rotate(axis), error, data.limit);
	}*/

	PxVec3 ra, rb;
	ch.prepareLockedAxes(cA2w.q, cB2w.q, cA2w.transformInv(cB2w.p), 7, 0, ra, rb);
	cA2wOut = ra + bA2w.p;
	cB2wOut = rb + bB2w.p;

	return ch.getCount();
}
#endif

void setupDesc(PxSolverContactDesc& contactDesc, const ImmediateScene::ImmediateActor* actors, PxSolverBodyData* solverBodyData, const PxU32 id, const bool aorb)
{
	PxTransform& bodyFrame = aorb ? contactDesc.bodyFrame1 : contactDesc.bodyFrame0;
	PxSolverConstraintPrepDescBase::BodyState& bodyState = aorb ? contactDesc.bodyState1 : contactDesc.bodyState0;
	const PxSolverBodyData*& data = aorb ? contactDesc.data1 : contactDesc.data0;

	Dy::ArticulationLinkHandle link = actors[id].mLink;
	if(link)
	{
		PxLinkData linkData;
		bool status = PxGetLinkData(link, linkData);
		PX_ASSERT(status);
		PX_UNUSED(status);

		data		= NULL;
		bodyFrame	= linkData.pose;
		bodyState	= PxSolverConstraintPrepDescBase::eARTICULATION;
	}
	else
	{
		data		= &solverBodyData[id];
		bodyFrame	= solverBodyData[id].body2World;
		bodyState	= actors[id].mType == ImmediateScene::ImmediateActor::eDYNAMIC ? PxSolverConstraintPrepDescBase::eDYNAMIC_BODY : PxSolverConstraintPrepDescBase::eSTATIC_BODY;
	}
}

void setupDesc(PxTGSSolverContactDesc& contactDesc, const ImmediateScene::ImmediateActor* actors, PxTGSSolverBodyTxInertia* txInertias, PxTGSSolverBodyData* solverBodyData, 
	PxTransform* poses, const PxU32 id, const bool aorb)
{
	PxTransform& bodyFrame = aorb ? contactDesc.bodyFrame1 : contactDesc.bodyFrame0;
	PxSolverConstraintPrepDescBase::BodyState& bodyState = aorb ? contactDesc.bodyState1 : contactDesc.bodyState0;
	const PxTGSSolverBodyData*& data = aorb ? contactDesc.bodyData1 : contactDesc.bodyData0;
	const PxTGSSolverBodyTxInertia*& txI = aorb ? contactDesc.body1TxI : contactDesc.body0TxI;

	Dy::ArticulationLinkHandle link = actors[id].mLink;
	if(link)
	{
		PxLinkData linkData;
		bool status = PxGetLinkData(link, linkData);
		PX_ASSERT(status);
		PX_UNUSED(status);

		data = NULL;
		txI = NULL;
		bodyFrame = linkData.pose;
		bodyState = PxSolverConstraintPrepDescBase::eARTICULATION;
	}
	else
	{
		data = &solverBodyData[id];
		txI = &txInertias[id];
		bodyFrame = poses[id];
		bodyState = actors[id].mType == ImmediateScene::ImmediateActor::eDYNAMIC ? PxSolverConstraintPrepDescBase::eDYNAMIC_BODY : PxSolverConstraintPrepDescBase::eSTATIC_BODY;
	}
}

void setupJointDesc(PxSolverConstraintPrepDesc& jointDesc, const ImmediateScene::ImmediateActor* actors, PxSolverBodyData* solverBodyData, const PxU32 bodyDataIndex, const bool aorb)
{
	if(!aorb)
		jointDesc.data0	= &solverBodyData[bodyDataIndex];
	else
		jointDesc.data1	= &solverBodyData[bodyDataIndex];

	PxTransform& bodyFrame = aorb ? jointDesc.bodyFrame1 : jointDesc.bodyFrame0;
	PxSolverConstraintPrepDescBase::BodyState& bodyState = aorb ? jointDesc.bodyState1 : jointDesc.bodyState0;

	if(actors[bodyDataIndex].mLink)
	{
		PxLinkData linkData;
		bool status = PxGetLinkData(actors[bodyDataIndex].mLink, linkData);
		PX_ASSERT(status);
		PX_UNUSED(status);

		bodyFrame	= linkData.pose;
		bodyState	= PxSolverConstraintPrepDescBase::eARTICULATION;
	}
	else
	{
		//This may seem redundant but the bodyFrame is not defined by the bodyData object when using articulations.
		// PT: TODO: this is a bug in the immediate mode snippet
		if(actors[bodyDataIndex].mType == ImmediateScene::ImmediateActor::eSTATIC)
		{
			bodyFrame	= PxTransform(PxIdentity);
			bodyState	= PxSolverConstraintPrepDescBase::eSTATIC_BODY;
		}
		else
		{
			bodyFrame	= solverBodyData[bodyDataIndex].body2World;
			bodyState	= PxSolverConstraintPrepDescBase::eDYNAMIC_BODY;
		}
	}
}

void setupJointDesc(PxTGSSolverConstraintPrepDesc& jointDesc, const ImmediateScene::ImmediateActor* actors, PxTGSSolverBodyTxInertia* txInertias, PxTGSSolverBodyData* solverBodyData, PxTransform* poses, const PxU32 bodyDataIndex, const bool aorb)
{
	if(!aorb)
	{
		jointDesc.bodyData0 = &solverBodyData[bodyDataIndex];
		jointDesc.body0TxI = &txInertias[bodyDataIndex];
	}
	else
	{
		jointDesc.bodyData1 = &solverBodyData[bodyDataIndex];
		jointDesc.body1TxI = &txInertias[bodyDataIndex];
	}

	PxTransform& bodyFrame = aorb ? jointDesc.bodyFrame1 : jointDesc.bodyFrame0;
	PxSolverConstraintPrepDescBase::BodyState& bodyState = aorb ? jointDesc.bodyState1 : jointDesc.bodyState0;

	if(actors[bodyDataIndex].mLink)
	{
		PxLinkData linkData;
		bool status = PxGetLinkData(actors[bodyDataIndex].mLink, linkData);
		PX_ASSERT(status);
		PX_UNUSED(status);

		bodyFrame = linkData.pose;
		bodyState = PxSolverConstraintPrepDescBase::eARTICULATION;
	}
	else
	{
		//This may seem redundant but the bodyFrame is not defined by the bodyData object when using articulations.
		// PT: TODO: this is a bug in the immediate mode snippet
		if(actors[bodyDataIndex].mType == ImmediateScene::ImmediateActor::eSTATIC)
		{
			bodyFrame = PxTransform(PxIdentity);
			bodyState = PxSolverConstraintPrepDescBase::eSTATIC_BODY;
		}
		else
		{
			bodyFrame = poses[bodyDataIndex];
			bodyState = PxSolverConstraintPrepDescBase::eDYNAMIC_BODY;
		}
	}
}

void ImmediateScene::createContactConstraints(float dt, float invDt, float lengthScale, const PxU32 nbPosIterations)
{
	//Only referenced if using TGS solver
	PX_UNUSED(lengthScale);
	PX_UNUSED(nbPosIterations);
#if USE_TGS
	const float stepDt = dt/float(nbPosIterations);
	const float stepInvDt = invDt*float(nbPosIterations);
#endif
#if BATCH_CONTACTS
	mHeaders.resize(mSolverConstraintDesc.size());

	const PxU32 nbBodies = mActors.size() - mNbStaticActors;

	mOrderedSolverConstraintDesc.resize(mSolverConstraintDesc.size());
	Array<PxSolverConstraintDesc>& orderedDescs = mOrderedSolverConstraintDesc;

#if USE_TGS
	const PxU32 nbContactHeaders = physx::immediate::PxBatchConstraintsTGS(	mSolverConstraintDesc.begin(), mContactPairs.size(), mSolverBodies.begin(), nbBodies,
																			mHeaders.begin(), orderedDescs.begin(),
																			mArticulations.begin(), mArticulations.size());

	//2 batch the joints...
	const PxU32 nbJointHeaders = physx::immediate::PxBatchConstraintsTGS(	mSolverConstraintDesc.begin() + mContactPairs.size(), mJointData.size(), mSolverBodies.begin(), nbBodies,
																			mHeaders.begin() + nbContactHeaders, orderedDescs.begin() + mContactPairs.size(),
																			mArticulations.begin(), mArticulations.size());
#else
	//1 batch the contacts
	const PxU32 nbContactHeaders = physx::immediate::PxBatchConstraints(mSolverConstraintDesc.begin(), mContactPairs.size(), mSolverBodies.begin(), nbBodies,
																		mHeaders.begin(), orderedDescs.begin(),
																		mArticulations.begin(), mArticulations.size());

	//2 batch the joints...
	const PxU32 nbJointHeaders = physx::immediate::PxBatchConstraints(	mSolverConstraintDesc.begin() + mContactPairs.size(), mJointData.size(), mSolverBodies.begin(), nbBodies,
																		mHeaders.begin() + nbContactHeaders, orderedDescs.begin() + mContactPairs.size(),
																		mArticulations.begin(), mArticulations.size());
#endif

	const PxU32 totalHeaders = nbContactHeaders + nbJointHeaders;

	mHeaders.forceSize_Unsafe(totalHeaders);
#else
	Array<PxSolverConstraintDesc>& orderedDescs = mSolverConstraintDesc;

	const PxU32 nbContactHeaders = mContactPairs.size();
	#ifdef TEST_IMMEDIATE_JOINTS
	const PxU32 nbJointHeaders = mJointData.size();
	PX_ASSERT(nbContactHeaders+nbJointHeaders==mSolverConstraintDesc.size());
	mHeaders.resize(nbContactHeaders+nbJointHeaders);
	#else
	PX_ASSERT(nbContactHeaders==mSolverConstraintDesc.size());
	PX_UNUSED(dt);
	mHeaders.resize(nbContactHeaders);
	#endif
	
	// We are bypassing the constraint batching so we create dummy PxConstraintBatchHeaders
	for(PxU32 i=0; i<nbContactHeaders; i++)
	{
		PxConstraintBatchHeader& hdr = mHeaders[i];
		hdr.startIndex = i;
		hdr.stride = 1;
		hdr.constraintType = PxSolverConstraintDesc::eCONTACT_CONSTRAINT;
	}
	#ifdef TEST_IMMEDIATE_JOINTS
	for(PxU32 i=0; i<nbJointHeaders; i++)
	{
		PxConstraintBatchHeader& hdr = mHeaders[nbContactHeaders+i];
		hdr.startIndex = i;
		hdr.stride = 1;
		hdr.constraintType = PxSolverConstraintDesc::eJOINT_CONSTRAINT;
	}
	#endif
#endif

	mContactForces.resize(mContactPoints.size());

	for(PxU32 i=0; i<nbContactHeaders; i++)
	{
		PxConstraintBatchHeader& header = mHeaders[i];
		PX_ASSERT(header.constraintType == PxSolverConstraintDesc::eCONTACT_CONSTRAINT);

#if USE_TGS
		PxTGSSolverContactDesc contactDescs[4];
#else
		PxSolverContactDesc contactDescs[4];
#endif

#if WITH_PERSISTENCY
		PersistentContactPair* persistentPairs[4];
#endif

		for(PxU32 a=0; a<header.stride; a++)
		{
			PxSolverConstraintDesc& constraintDesc = orderedDescs[header.startIndex + a];
			
			//Extract the contact pair that we saved in this structure earlier.
			const ContactPair& pair = *reinterpret_cast<const ContactPair*>(constraintDesc.constraint);
#if USE_TGS
			PxTGSSolverContactDesc& contactDesc = contactDescs[a];

			setupDesc(contactDesc, mActors.begin(), mSolverBodyTxInertias.begin(), mSolverBodyData.begin(), mPoses.begin(), pair.mID0, false);
			setupDesc(contactDesc, mActors.begin(), mSolverBodyTxInertias.begin(), mSolverBodyData.begin(), mPoses.begin(), pair.mID1, true);

			contactDesc.body0					= constraintDesc.tgsBodyA;
			contactDesc.body1					= constraintDesc.tgsBodyB;

			contactDesc.torsionalPatchRadius	= 0.0f;
			contactDesc.minTorsionalPatchRadius	= 0.0f;
#else
			PxSolverContactDesc& contactDesc = contactDescs[a];

			setupDesc(contactDesc, mActors.begin(), mSolverBodyData.begin(), pair.mID0, false);
			setupDesc(contactDesc, mActors.begin(), mSolverBodyData.begin(), pair.mID1, true);

			contactDesc.body0					= constraintDesc.bodyA;
			contactDesc.body1					= constraintDesc.bodyB;
#endif
			contactDesc.contactForces			= &mContactForces[pair.mStartContactIndex];
			contactDesc.contacts				= &mContactPoints[pair.mStartContactIndex];
			contactDesc.numContacts				= pair.mNbContacts;

#if WITH_PERSISTENCY
			const HashMap<IDS, PersistentContactPair>::Entry* e = mPersistentPairs.find(IDS(pair.mID0, pair.mID1));
			PX_ASSERT(e);
			{
				PersistentContactPair& pcp = const_cast<PersistentContactPair&>(e->second);
				contactDesc.frictionPtr			= pcp.frictions;
				contactDesc.frictionCount		= PxU8(pcp.nbFrictions);
				persistentPairs[a]				= &pcp;
			}
#else
			contactDesc.frictionPtr				= NULL;
			contactDesc.frictionCount			= 0;
#endif
			contactDesc.disableStrongFriction = false;
			contactDesc.hasMaxImpulse			= false;
			contactDesc.hasForceThresholds		= false;
			contactDesc.shapeInteraction		= NULL;
			contactDesc.restDistance			= 0.0f;
			contactDesc.maxCCDSeparation		= PX_MAX_F32;

			contactDesc.desc					= &constraintDesc;
			contactDesc.invMassScales.angular0 = contactDesc.invMassScales.angular1 = contactDesc.invMassScales.linear0 = contactDesc.invMassScales.linear1 = 1.0f;
		}

#if USE_TGS
		PxCreateContactConstraintsTGS(&header, 1, contactDescs, *mConstraintAllocator, stepInvDt, invDt, gBounceThreshold, gFrictionOffsetThreshold, gCorrelationDistance);
#else
		PxCreateContactConstraints(&header, 1, contactDescs, *mConstraintAllocator, invDt, gBounceThreshold, gFrictionOffsetThreshold, gCorrelationDistance);
#endif

#if WITH_PERSISTENCY
		//Cache friction information...
		for (PxU32 a = 0; a < header.stride; ++a)
		{
#if USE_TGS
			const PxTGSSolverContactDesc& contactDesc = contactDescs[a];
#else
			const PxSolverContactDesc& contactDesc = contactDescs[a];
#endif
			PersistentContactPair& pcp = *persistentPairs[a];
			pcp.frictions = contactDesc.frictionPtr;
			pcp.nbFrictions = contactDesc.frictionCount;
		}
#endif
	}

#ifdef TEST_IMMEDIATE_JOINTS
	for(PxU32 i=0; i<nbJointHeaders; i++)
	{
		PxConstraintBatchHeader& header = mHeaders[nbContactHeaders+i];
		PX_ASSERT(header.constraintType == PxSolverConstraintDesc::eJOINT_CONSTRAINT);

		{
#if USE_TGS
			PxTGSSolverConstraintPrepDesc jointDescs[4];
#else
			PxSolverConstraintPrepDesc jointDescs[4];
#endif
			PxImmediateConstraint constraints[4];

			header.startIndex += mContactPairs.size();

			for(PxU32 a=0; a<header.stride; a++)
			{
				PxSolverConstraintDesc& constraintDesc = orderedDescs[header.startIndex + a];
				//Extract the contact pair that we saved in this structure earlier.
				const MyJointData& jd = *reinterpret_cast<const MyJointData*>(constraintDesc.constraint);

				constraints[a].prep = SphericalJointSolverPrep;
				constraints[a].constantBlock = &jd;
#if USE_TGS
				PxTGSSolverConstraintPrepDesc& jointDesc = jointDescs[a];
				jointDesc.body0 = constraintDesc.tgsBodyA;
				jointDesc.body1 = constraintDesc.tgsBodyB;
				setupJointDesc(jointDesc, mActors.begin(), mSolverBodyTxInertias.begin(), mSolverBodyData.begin(), mPoses.begin(), constraintDesc.bodyADataIndex, false);
				setupJointDesc(jointDesc, mActors.begin(), mSolverBodyTxInertias.begin(), mSolverBodyData.begin(), mPoses.begin(), constraintDesc.bodyBDataIndex, true);
#else
				PxSolverConstraintPrepDesc& jointDesc = jointDescs[a];
				jointDesc.body0 = constraintDesc.bodyA;
				jointDesc.body1 = constraintDesc.bodyB;
				setupJointDesc(jointDesc, mActors.begin(), mSolverBodyData.begin(), constraintDesc.bodyADataIndex, false);
				setupJointDesc(jointDesc, mActors.begin(), mSolverBodyData.begin(), constraintDesc.bodyBDataIndex, true);
#endif				
				jointDesc.desc					= &constraintDesc;
				jointDesc.writeback				= NULL;
				jointDesc.linBreakForce			= PX_MAX_F32;
				jointDesc.angBreakForce			= PX_MAX_F32;
				jointDesc.minResponseThreshold	= 0;
				jointDesc.disablePreprocessing	= false;
				jointDesc.improvedSlerp			= false;
				jointDesc.driveLimitsAreForces	= false;
				jointDesc.invMassScales.angular0 = jointDesc.invMassScales.angular1 = jointDesc.invMassScales.linear0 = jointDesc.invMassScales.linear1 = 1.0f;
			}

#if USE_TGS
			immediate::PxCreateJointConstraintsWithImmediateShadersTGS(&header, 1, constraints, jointDescs, *mConstraintAllocator, stepDt, dt, stepInvDt, invDt, lengthScale);
#else
			immediate::PxCreateJointConstraintsWithImmediateShaders(&header, 1, constraints, jointDescs, *mConstraintAllocator, dt, invDt);
#endif
		}
	}
#endif
}

void ImmediateScene::solveAndIntegrate(float dt)
{
#ifdef PRINT_TIMINGS
	unsigned long long time0 = __rdtsc();
#endif
	const PxU32 totalNbActors = mActors.size();
	const PxU32 nbDynamicActors = totalNbActors - mNbStaticActors - mNbArticulationLinks;
	const PxU32 nbDynamic = nbDynamicActors + mNbArticulationLinks;

	mMotionLinearVelocity.resize(nbDynamic);
	mMotionAngularVelocity.resize(nbDynamic);

	const PxU32 nbArticulations = mArticulations.size();
	Dy::ArticulationV** articulations = mArticulations.begin();

#if USE_TGS
	const float stepDt = dt/float(gNbIterPos);

	immediate::PxSolveConstraintsTGS(mHeaders.begin(), mHeaders.size(),
#if BATCH_CONTACTS
		mOrderedSolverConstraintDesc.begin(),
#else
		mSolverConstraintDesc.begin(),
#endif
		mSolverBodies.begin(), mSolverBodyTxInertias.begin(),
		nbDynamic, gNbIterPos, gNbIterVel, stepDt, 1.0f / stepDt, nbArticulations, articulations);
#else

	PxMemZero(mSolverBodies.begin(), mSolverBodies.size() * sizeof(PxSolverBody));

	PxSolveConstraints(	mHeaders.begin(), mHeaders.size(),
#if BATCH_CONTACTS
						mOrderedSolverConstraintDesc.begin(),
#else
						mSolverConstraintDesc.begin(),
#endif
						mSolverBodies.begin(),
						mMotionLinearVelocity.begin(), mMotionAngularVelocity.begin(), nbDynamic, gNbIterPos, gNbIterVel,
						dt, 1.0f/dt, nbArticulations, articulations);
#endif

#ifdef PRINT_TIMINGS
	unsigned long long time1 = __rdtsc();
#endif

#if USE_TGS
	PxIntegrateSolverBodiesTGS(mSolverBodies.begin(), mSolverBodyTxInertias.begin(), mPoses.begin(), nbDynamicActors, dt);
	for (PxU32 i = 0; i<nbArticulations; i++)
		PxUpdateArticulationBodiesTGS(articulations[i], dt);

	for (PxU32 i = 0; i<nbDynamicActors; i++)
	{
		PX_ASSERT(mActors[i].mType == ImmediateActor::eDYNAMIC);
		const PxTGSSolverBodyVel& data = mSolverBodies[i];
		mActors[i].mLinearVelocity = data.linearVelocity;
		mActors[i].mAngularVelocity = data.angularVelocity;
	}

#else
	PxIntegrateSolverBodies(mSolverBodyData.begin(), mSolverBodies.begin(), mMotionLinearVelocity.begin(), mMotionAngularVelocity.begin(), nbDynamicActors, dt);
	for (PxU32 i = 0; i<nbArticulations; i++)
		PxUpdateArticulationBodies(articulations[i], dt);

	for (PxU32 i = 0; i<nbDynamicActors; i++)
	{
		PX_ASSERT(mActors[i].mType == ImmediateActor::eDYNAMIC);
		const PxSolverBodyData& data = mSolverBodyData[i];
		mActors[i].mLinearVelocity = data.linearVelocity;
		mActors[i].mAngularVelocity = data.angularVelocity;
		mPoses[i] = data.body2World;
	}
#endif

	for(PxU32 i=0;i<mNbArticulationLinks;i++)
	{
		const PxU32 j = nbDynamicActors + i;
		PX_ASSERT(mActors[j].mType==ImmediateActor::eLINK);

		PxLinkData data;
		bool status = PxGetLinkData(mActors[j].mLink, data);
		PX_ASSERT(status);
		PX_UNUSED(status);

		mActors[j].mLinearVelocity = data.linearVelocity;
		mActors[j].mAngularVelocity = data.angularVelocity;
		mPoses[j] = data.pose;
	}

#ifdef PRINT_TIMINGS
	unsigned long long time2 = __rdtsc();
	printf("solve: %d           \n", (time1-time0)/1024);
	printf("integrate: %d           \n", (time2-time1)/1024);
#endif
}

static PxDefaultAllocator		gAllocator;
static PxDefaultErrorCallback	gErrorCallback;
static PxFoundation*			gFoundation	= NULL;
static ImmediateScene*			gScene		= NULL;

///////////////////////////////////////////////////////////////////////////////

PxU32 getNbGeoms()
{
	return gScene ? gScene->mGeoms.size() : 0;
}

const PxGeometryHolder* getGeoms()
{
	if(!gScene || !gScene->mGeoms.size())
		return NULL;
	return &gScene->mGeoms[0];
}

const PxTransform* getGeomPoses()
{
	if(!gScene || !gScene->mPoses.size())
		return NULL;
	return &gScene->mPoses[0];
}

PxU32 getNbArticulations()
{
	return gScene ? gScene->mArticulations.size() : 0;
}

Dy::ArticulationV** getArticulations()
{
	if(!gScene || !gScene->mArticulations.size())
		return NULL;
	return &gScene->mArticulations[0];
}

PxU32 getNbBounds()
{
	if(!gDrawBounds)
		return 0;
	return gScene ? gScene->mBounds.size() : 0;
}

const PxBounds3* getBounds()
{
	if(!gDrawBounds)
		return NULL;
	if(!gScene || !gScene->mBounds.size())
		return NULL;
	return &gScene->mBounds[0];
}

PxU32 getNbContacts()
{
	return gScene ? gScene->mContactPoints.size() : 0;
}

const Gu::ContactPoint* getContacts()
{
	if(!gScene || !gScene->mContactPoints.size())
		return NULL;
	return &gScene->mContactPoints[0];
}

///////////////////////////////////////////////////////////////////////////////
class CustomProfilerCallback2 : public physx::PxProfilerCallback
{
public:
	virtual ~CustomProfilerCallback2() {}

	virtual void* zoneStart(const char* eventName, bool detached, uint64_t contextId)
	{
		b3EnterProfileZone(eventName);
		return 0;
	}

	virtual void zoneEnd(void* profilerData, const char* eventName, bool detached, uint64_t contextId)
	{
		b3LeaveProfileZone();
	}

};

static CustomProfilerCallback2 gCustomProfilerCallback;



void initPhysics(bool /*interactive*/)
{
	gFoundation = PxCreateFoundation(PX_PHYSICS_VERSION, gAllocator, gErrorCallback);

	PxSetProfilerCallback(&gCustomProfilerCallback);
	PxRegisterImmediateArticulations();
	
	gScene = new ImmediateScene;
	gScene->createScene();
}

void stepPhysics(bool /*interactive*/)
{
	PX_PROFILE_ZONE("stepPhysics", 0);
	if(!gScene)
		return;

	if(gPause && !gOneFrame)
		return;
	gOneFrame = false;

	const float dt = 1.0f/60.0f;
	const float invDt = 60.0f;

	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("updateArticulations", 0);
		gScene->updateArticulations(dt);
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("updateArticulations: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("updateBounds", 0);
		gScene->updateBounds();
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("updateBounds: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("broadPhase", 0);
		gScene->broadPhase();
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("broadPhase: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("narrowPhase", 0);
		gScene->narrowPhase();
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("narrowPhase: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("buildSolverBodyData", 0);
		gScene->buildSolverBodyData(dt);
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("buildSolverBodyData: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("buildSolverConstraintDesc", 0);
		gScene->buildSolverConstraintDesc();
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("buildSolverConstraintDesc: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("createContactConstraints", 0);
		gScene->createContactConstraints(dt, invDt, 1.f, gNbIterPos);
#ifdef PRINT_TIMINGS
		time = __rdtsc() - time;
		printf("createContactConstraints: %d           \n", time/1024);
#endif
	}
	{
#ifdef PRINT_TIMINGS
//		unsigned long long time = __rdtsc();
#endif
		PX_PROFILE_ZONE("solveAndIntegrate", 0);
		gScene->solveAndIntegrate(dt);
#ifdef PRINT_TIMINGS
//		time = __rdtsc() - time;
//		printf("solveAndIntegrate: %d           \n", time/1024);
#endif
	}

	if(gScene->mMotorLink)
	{
		PX_PROFILE_ZONE("mMotorLink", 0);
		static float time = 0.0f;
		time += 0.1f;
		const float target = sinf(time) * 4.0f;
//		printf("target: %f\n", target);

		PxFeatherstoneArticulationJointData data;
		bool status = PxGetJointData(gScene->mMotorLink, data);
		PX_ASSERT(status);

		data.targetVel[PxArticulationAxis::eTWIST] = target;

		const PxVec3 boxExtents(0.5f, 0.1f, 0.5f);
		const float s = boxExtents.x*1.1f + fabsf(sinf(time))*0.5f;

		data.parentPose	= PxTransform(PxVec3(0.0f, 0.0f, s));
		data.childPose	= PxTransform(PxVec3(0.0f, 0.0f, -s));

		status = PxSetJointData(gScene->mMotorLink, data);
		PX_ASSERT(status);
		PX_UNUSED(status);
	}

}

void cleanupPhysics(bool /*interactive*/)
{
	PX_DELETE_AND_RESET(gScene);
	PX_RELEASE(gFoundation);

	printf("SnippetImmediateArticulation done.\n");
}

void keyPress(unsigned char key, const PxTransform& /*camera*/)
{
	if (key == 'c' || key == 'C')
		printContacts = !printContacts;

	if(key=='b' || key=='B')
		gDrawBounds = !gDrawBounds;

	//if(key=='p' || key=='P')
	//	gPause = !gPause;

	if(key=='o' || key=='O')
	{
		gPause = true;
		gOneFrame = true;
	}

	if(gScene)
	{
		if(key>=0 && key<=5)
		{
			gSceneIndex = key;
			gScene->reset();
			gScene->createScene();
		}

		if(key=='r' || key=='R')
		{
			gScene->reset();
			gScene->createScene();
		}
	}
}

int snippetMain3(int, const char*const*)
{
	printf("Immediate articulation snippet. Use these keys:\n");
	printf(" P        - enable/disable pause\n");
	printf(" O        - step simulation for one frame\n");
	printf(" R        - reset scene\n");
	printf(" F1 to F6 - select scene\n");
	printf("\n");
#ifdef RENDER_SNIPPET
	extern void renderLoop();
	renderLoop();
#else
	static const PxU32 frameCount = 100;
	initPhysics(false);

	for (PxU32 i = 0; i < frameCount; i++)
	{
		
		stepPhysics(false);
	}
	cleanupPhysics(false);
#endif

	return 0;
}

