/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#include "Detour/APS_DetourNode.h"

#include "Detour/DetourAssert.h"
#include "Detour/DetourCommon.h"

namespace APSDetour
{
#if USE_64BIT_ADDRESS

static inline unsigned int dtHashRef(dtPolyRef a)
{
	a += ~(a << 31);
	a ^=  (a >> 20);
	a +=  (a << 6);
	a ^=  (a >> 12);
	a += ~(a << 22);
	a ^=  (a >> 32);
	return (unsigned int)a;
}

#else

static inline unsigned int dtHashRef(dtPolyRef a)
{
	a += ~(a << 15);
	a ^=  (a >> 10);
	a +=  (a << 3);
	a ^=  (a >> 6);
	a += ~(a << 11);
	a ^=  (a >> 16);
	return (unsigned int)a;
}

#endif // USE_64BIT_ADDRESS

/////////////////////////////////////////////////////////////////////////////////

dtNodePool::dtNodePool(int maxNodes, int hashSize)
	: m_nodes(nullptr)
	, m_first(nullptr)
	, m_next(nullptr)
	, m_maxNodes(maxNodes)
	, m_hashSize(hashSize)
	, m_maxRuntimeNodes(maxNodes)
	, m_nodeCount(0)
{
	dtAssert(dtNextPow2(m_hashSize) == (unsigned int)m_hashSize);
	dtAssert(m_maxNodes > 0);

	// We use the UE allocator instead of dtAlloc/dtFree so we don't depend on the Detour API differences between UE4 and UE5.
	m_nodes = static_cast<dtNode*>(FMemory::Malloc(sizeof(dtNode) * m_maxNodes));
	m_next  = static_cast<dtNodeIndex*>(FMemory::Malloc(sizeof(dtNodeIndex) * m_maxNodes));
	m_first = static_cast<dtNodeIndex*>(FMemory::Malloc(sizeof(dtNodeIndex) * m_hashSize));

	dtAssert(m_nodes);
	dtAssert(m_next);
	dtAssert(m_first);

	memset(m_first, 0xff, sizeof(dtNodeIndex) * m_hashSize);
	memset(m_next,  0xff, sizeof(dtNodeIndex) * m_maxNodes);
}

dtNodePool::~dtNodePool()
{
	FMemory::Free(m_nodes);
	FMemory::Free(m_next);
	FMemory::Free(m_first);
}

void dtNodePool::clear()
{
	memset(m_first, 0xff, sizeof(dtNodeIndex) * m_hashSize);
	m_nodeCount = 0;
}

dtNode* dtNodePool::findNode(dtPolyRef id)
{
	const unsigned int bucket = dtHashRef(id) & (m_hashSize - 1);
	dtNodeIndex i = m_first[bucket];
	while (i != DT_NULL_IDX)
	{
		if (m_nodes[i].id == id)
			return &m_nodes[i];
		i = m_next[i];
	}
	return nullptr;
}

dtNode* dtNodePool::getNode(dtPolyRef id)
{
	const unsigned int bucket = dtHashRef(id) & (m_hashSize - 1);
	dtNodeIndex i = m_first[bucket];

	while (i != DT_NULL_IDX)
	{
		if (m_nodes[i].id == id)
			return &m_nodes[i];
		i = m_next[i];
	}

	if (m_nodeCount >= m_maxRuntimeNodes)
	{
		// The pool has been exhausted within the specified runtime limit.
		return nullptr;
	}

	i = (dtNodeIndex)m_nodeCount;
	m_nodeCount++;

	// Init node
	dtNode* node = &m_nodes[i];
	node->pidx  = 0;
	node->cost  = 0.0f;
	node->total = 0.0f;
	node->id    = id;
	node->flags = 0;
	// We don't initialize pos here — the calling code sets it as needed.

	m_next[i] = m_first[bucket];
	m_first[bucket] = i;

	return node;
}

//////////////////////////////////////////////////////////////////////////////////////////

dtNodeQueue::dtNodeQueue(int n)
	: m_heap(nullptr)
	, m_capacity(n)
	, m_size(0)
{
	dtAssert(m_capacity > 0);

	// We use FMemory so we don't depend on dtAlloc/dtFree
	m_heap = static_cast<dtNode**>(
		FMemory::Malloc(sizeof(dtNode*) * (m_capacity + 1))
	);

	dtAssert(m_heap);
}

dtNodeQueue::~dtNodeQueue()
{
	FMemory::Free(m_heap);
}

void dtNodeQueue::bubbleUp(int i, dtNode* node)
{
	int parent = (i - 1) / 2;
	// note: (index > 0) means there is a parent
	while ((i > 0) && (m_heap[parent]->total > node->total))
	{
		m_heap[i] = m_heap[parent];
		i = parent;
		parent = (i - 1) / 2;
	}
	m_heap[i] = node;
}

void dtNodeQueue::trickleDown(int i, dtNode* node)
{
	int child = (i * 2) + 1;
	while (child < m_size)
	{
		if (((child + 1) < m_size) &&
			(m_heap[child]->total > m_heap[child + 1]->total))
		{
			child++;
		}
		m_heap[i] = m_heap[child];
		i = child;
		child = (i * 2) + 1;
	}
	bubbleUp(i, node);
}
}