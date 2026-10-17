/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "Detour/DetourNavMesh.h"

// We keep the same types/names as in the original to minimize breaking the code.
namespace APSDetour
{
enum dtNodeFlags
{
	DT_NODE_OPEN   = 0x01,
	DT_NODE_CLOSED = 0x02,
};

typedef unsigned short dtNodeIndex;
static const dtNodeIndex DT_NULL_IDX = (dtNodeIndex)~0;

struct dtNode
{
	float pos[3];              ///< Position of the node.
	float cost;                ///< Cost from previous node to current node.
	float total;               ///< Cost up to the node.
	unsigned int pidx  : 30;   ///< Index to parent node.
	unsigned int flags : 2;    ///< Node flags 0/open/closed.
	dtPolyRef id;              ///< Polygon ref the node corresponds to.
};

class dtNodePool
{
public:
	dtNodePool(int maxNodes, int hashSize);
	~dtNodePool();

	inline void operator=(const dtNodePool&) {}

	void clear();
	dtNode* getNode(dtPolyRef id);
	dtNode* findNode(dtPolyRef id);

	inline unsigned int getNodeIdx(const dtNode* node) const
	{
		if (!node) return 0;
		return (unsigned int)(node - m_nodes) + 1;
	}

	inline dtNode* getNodeAtIdx(unsigned int idx)
	{
		if (!idx) return nullptr;
		return &m_nodes[idx - 1];
	}

	inline const dtNode* getNodeAtIdx(unsigned int idx) const
	{
		if (!idx) return nullptr;
		return &m_nodes[idx - 1];
	}

	inline int getMemUsed() const
	{
		return sizeof(*this) +
			sizeof(dtNode) * m_maxNodes +
			sizeof(dtNodeIndex) * m_maxNodes +
			sizeof(dtNodeIndex) * m_hashSize;
	}

	inline int getMaxNodes() const { return m_maxNodes; }

	// If we use a shared query instance, we can limit the runtime pool size.
	inline int getMaxRuntimeNodes() const { return m_maxRuntimeNodes; }

	inline int getNodeCount() const { return m_nodeCount; }

	inline int getHashSize() const { return m_hashSize; }
	inline dtNodeIndex getFirst(int bucket) const { return m_first[bucket]; }
	inline dtNodeIndex getNext(int i) const { return m_next[i]; }

	// overrides m_maxNodes for runtime purposes
	inline void setMaxRuntimeNodes(const int newMaxRuntimeNodes) { m_maxRuntimeNodes = newMaxRuntimeNodes; }

private:
	dtNode*      m_nodes;
	dtNodeIndex* m_first;
	dtNodeIndex* m_next;
	const int    m_maxNodes;
	const int    m_hashSize;
	int          m_maxRuntimeNodes;
	int          m_nodeCount;
};

class dtNodeQueue
{
public:
	dtNodeQueue(int n);
	~dtNodeQueue();

	inline void operator=(dtNodeQueue&) {}

	inline void clear()
	{
		m_size = 0;
	}

	inline dtNode* top()
	{
		return m_heap[0];
	}

	inline dtNode* pop()
	{
		dtNode* result = m_heap[0];
		m_size--;
		trickleDown(0, m_heap[m_size]);
		return result;
	}

	inline void push(dtNode* node)
	{
		m_size++;
		bubbleUp(m_size - 1, node);
	}

	inline void modify(dtNode* node)
	{
		for (int i = 0; i < m_size; ++i)
		{
			if (m_heap[i] == node)
			{
				bubbleUp(i, node);
				return;
			}
		}
	}

	inline bool empty() const { return m_size == 0; }

	inline int getMemUsed() const
	{
		return sizeof(*this) +
			sizeof(dtNode*) * (m_capacity + 1);
	}

	inline int getCapacity() const { return m_capacity; }

private:
	void bubbleUp(int i, dtNode* node);
	void trickleDown(int i, dtNode* node);

	dtNode**   m_heap;
	const int  m_capacity;
	int        m_size;
};
}