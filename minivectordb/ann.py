"""Self-implemented IVF (inverted file) approximate index.

The index clusters stored vectors with k-means and answers ann queries by
probing the closest clusters, then re-ranking candidates exactly.  Cluster
memberships are rebuilt from the persisted table records (the index itself is
never authoritative), which keeps recovery simple and transactions safe: any
committed write marks the index dirty and the next ann query rebuilds it from
current rows.  Small tables answer ann queries with an exact scan, which is a
legal approximation output and keeps tiny public cases exactly ordered.
"""

import math

import numpy as np

from vectors import l2_distance

# Tables with at most this many rows answer ann queries by exact scanning.
EXACT_SCAN_THRESHOLD = 2048
_KMEANS_ITERATIONS = 12
_KMEANS_SAMPLE = 16384


def _choose_clusters(n):
    return max(8, min(128, int(2 * math.sqrt(max(n, 1)))))


class IVFIndex:
    def __init__(self, dim, centroids, cluster_pks, pk_cluster):
        self.dim = dim
        self.centroids = centroids  # (k, dim) float64
        self.cluster_pks = cluster_pks  # list of lists of pk
        self.pk_cluster = pk_cluster  # dict pk -> cluster id

    @classmethod
    def build(cls, dim, items):
        """Build from ``items``: iterable of (pk, float32 vector)."""
        pks = []
        vectors = []
        for pk, vector in items:
            pks.append(pk)
            vectors.append(vector)
        if not pks:
            return cls(dim, np.zeros((0, dim)), [], {})
        matrix = np.vstack([np.asarray(v, dtype=np.float32) for v in vectors]).astype(np.float64)
        n = len(pks)
        k = min(_choose_clusters(n), n)
        rng = np.random.default_rng(20261118)
        sample = matrix if n <= _KMEANS_SAMPLE else matrix[rng.choice(n, _KMEANS_SAMPLE, replace=False)]
        centroids = sample[rng.choice(len(sample), k, replace=False)].copy()
        sample_norms = (sample * sample).sum(axis=1)
        for _ in range(_KMEANS_ITERATIONS):
            # |x-c|^2 = |x|^2 - 2 x.c + |c|^2, computed without big temporaries.
            distances = sample_norms[:, None] - 2.0 * (sample @ centroids.T) \
                + (centroids * centroids).sum(axis=1)[None, :]
            assignment = distances.argmin(axis=1)
            moved = False
            for cluster in range(k):
                members = sample[assignment == cluster]
                if len(members):
                    new_center = members.mean(axis=0)
                    if not np.array_equal(new_center, centroids[cluster]):
                        centroids[cluster] = new_center
                        moved = True
            if not moved:
                break
        # Assign every point to its final centroid, chunked.
        cluster_pks = [[] for _ in range(len(centroids))]
        pk_cluster = {}
        chunk = 4096
        centroid_norms = (centroids * centroids).sum(axis=1)
        for start in range(0, n, chunk):
            block = matrix[start:start + chunk]
            block_norms = (block * block).sum(axis=1)
            distances = block_norms[:, None] - 2.0 * (block @ centroids.T) + centroid_norms[None, :]
            nearest = distances.argmin(axis=1)
            for offset, cluster in enumerate(nearest):
                pk = pks[start + offset]
                cluster_pks[cluster].append(pk)
                pk_cluster[pk] = int(cluster)
        return cls(dim, centroids, cluster_pks, pk_cluster)

    def search(self, query, k, nprobe_ratio, fetch_vector):
        """Return up to ``k`` (distance, pk) pairs, sorted by (distance, pk)."""
        if not self.cluster_pks:
            return []
        query = np.asarray(query, dtype=np.float64)
        centroid_distances = ((self.centroids - query) ** 2).sum(axis=1)
        total = len(self.centroids)
        nprobe = min(total, max(1, int(total * nprobe_ratio)))
        probe_order = np.argsort(centroid_distances, kind="stable")
        candidates = []
        seen = 0
        for cluster in probe_order[:nprobe]:
            candidates.extend(self.cluster_pks[cluster])
            seen += len(self.cluster_pks[cluster])
        while len(candidates) < k and nprobe < total:
            cluster = probe_order[nprobe]
            candidates.extend(self.cluster_pks[cluster])
            nprobe += 1
        results = []
        for pk in candidates:
            vector = fetch_vector(pk)
            if vector is None:
                continue  # row deleted since the index was built
            results.append((l2_distance(vector, query), pk))
        results.sort()
        return results[:k]

    def search_filtered(self, query, k, nprobe_ratio, fetch_vector, allowed):
        """Like ``search`` but only pks passing ``allowed`` (a set) count."""
        if not self.cluster_pks:
            return []
        query = np.asarray(query, dtype=np.float64)
        centroid_distances = ((self.centroids - query) ** 2).sum(axis=1)
        total = len(self.centroids)
        nprobe = min(total, max(1, int(total * nprobe_ratio)))
        probe_order = np.argsort(centroid_distances, kind="stable")
        results = []
        nprobe_used = 0
        for cluster in probe_order:
            if nprobe_used >= nprobe and len(results) >= k:
                break
            for pk in self.cluster_pks[cluster]:
                if pk not in allowed:
                    continue
                vector = fetch_vector(pk)
                if vector is None:
                    continue
                results.append((l2_distance(vector, query), pk))
            nprobe_used += 1
        results.sort()
        return results[:k]
