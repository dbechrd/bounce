/*
* Copyright (c) 2016-2019 Irlan Robson https://irlanrobson.github.io
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*/

#include <bounce/softbody/softbody_solver.h>
#include <bounce/softbody/softbody_mesh.h>
#include <bounce/softbody/softbody_node.h>
#include <bounce/softbody/softbody.h>
#include <bounce/softbody/softbody_contact_solver.h>

#include <bounce/dynamics/body.h>
#include <bounce/dynamics/shapes/shape.h>

#include <bounce/cloth/sparse_mat33.h>
#include <bounce/cloth/sparse_mat33_view.h>

// This work is based on the paper "Interactive Virtual Materials" written by 
// Matthias Mueller Fischer
// The paper is available here:
// http://matthias-mueller-fischer.ch/publications/GI2004.pdf

// In order to support velocity constraints on node velocities 
// we solve Ax = b using a Modified Preconditioned Conjugate Gradient (MPCG) algorithm.

// Number of MPCG iterations, a value that is normally small when small time steps are taken.
u32 b3_softBodySolverIterations = 0;

// Enables the stiffness warping solver.
bool b3_enableStiffnessWarping = true;

b3SoftBodySolver::b3SoftBodySolver(const b3SoftBodySolverDef& def)
{
	m_body = def.body;
	m_allocator = &m_body->m_stackAllocator;
	m_mesh = m_body->m_mesh;
	m_nodes = m_body->m_nodes;
	m_elements = m_body->m_elements;
}

b3SoftBodySolver::~b3SoftBodySolver()
{

}

// https://animation.rwth-aachen.de/media/papers/2016-MIG-StableRotation.pdf
static void b3ExtractRotation(b3Mat33& out, b3Quat& q, const b3Mat33& A, u32 maxIterations = 20)
{
	for (u32 iteration = 0; iteration < maxIterations; ++iteration)
	{
		b3Mat33 R = b3QuatMat33(q);

		float32 s = b3Abs(b3Dot(R.x, A.x) + b3Dot(R.y, A.y) + b3Dot(R.z, A.z));

		if (s == 0.0f)
		{
			break;
		}

		float32 inv_s = 1.0f / s + 1.0e-9f;

		b3Vec3 v = b3Cross(R.x, A.x) + b3Cross(R.y, A.y) + b3Cross(R.z, A.z);

		b3Vec3 omega = inv_s * v;

		float32 w = b3Length(omega);

		if (w < 1.0e-9f)
		{
			break;
		}

		b3Quat omega_q(omega / w, w);

		q = omega_q * q;
		q.Normalize();
	}

	out = b3QuatMat33(q);
}

static void b3SolveMPCG(b3DenseVec3& x,
	const b3SparseMat33View& A, const b3DenseVec3& b,
	const b3DenseVec3& z, const b3DiagMat33& S, u32 maxIterations = 20)
{
	B3_PROFILE("Soft Body Solve Ax = b");

	b3DiagMat33 P(A.rowCount);
	b3DiagMat33 invP(A.rowCount);
	for (u32 i = 0; i < A.rowCount; ++i)
	{
		b3Mat33 D = A(i, i);

		// Sylvester Criterion to ensure PD-ness
		B3_ASSERT(b3Det(D.x, D.y, D.z) > 0.0f);

		B3_ASSERT(D.x.x != 0.0f);
		float32 xx = 1.0f / D.x.x;

		B3_ASSERT(D.y.y != 0.0f);
		float32 yy = 1.0f / D.y.y;

		B3_ASSERT(D.z.z != 0.0f);
		float32 zz = 1.0f / D.z.z;

		P[i] = b3Diagonal(xx, yy, zz);
		invP[i] = b3Diagonal(D.x.x, D.y.y, D.z.z);
	}

	x = z;

	float32 delta_0 = b3Dot(S * b, P * (S * b));

	b3DenseVec3 r = S * (b - A * x);
	b3DenseVec3 c = S * (invP * r);

	float32 delta_new = b3Dot(r, c);

	u32 iteration = 0;
	for (;;)
	{
		if (iteration == maxIterations)
		{
			break;
		}

		if (delta_new <= B3_EPSILON * B3_EPSILON * delta_0)
		{
			break;
		}

		b3DenseVec3 q = S * (A * c);

		float32 alpha = delta_new / b3Dot(c, q);

		x = x + alpha * c;
		r = r - alpha * q;

		b3DenseVec3 s = invP * r;

		float32 delta_old = delta_new;

		delta_new = b3Dot(r, s);

		float32 beta = delta_new / delta_old;

		c = S * (s + beta * c);

		++iteration;
	}

	b3_softBodySolverIterations = iteration;
}

static B3_FORCE_INLINE void b3Mul(float32* C, float32* A, u32 AM, u32 AN, float32* B, u32 BM, u32 BN)
{
	B3_ASSERT(AN == BM);

	for (u32 i = 0; i < AM; ++i)
	{
		for (u32 j = 0; j < BN; ++j)
		{
			C[i + AM * j] = 0.0f;

			for (u32 k = 0; k < AN; ++k)
			{
				C[i + AM * j] += A[i + AM * k] * B[k + BM * j];
			}
		}
	}
}

static B3_FORCE_INLINE float32 b3Length(float32* a, u32 an)
{
	float32 result = 0.0f;
	for (u32 i = 0; i < an; ++i)
	{
		result += a[i] * a[i];
	}
	return b3Sqrt(result);
}

void b3SoftBodySolver::Solve(float32 dt, const b3Vec3& gravity, u32 velocityIterations, u32 positionIterations)
{
	float32 h = dt;
	float32 inv_h = 1.0f / h;

	b3SparseMat33 M(m_mesh->vertexCount);
	b3SparseMat33 C(m_mesh->vertexCount);
	b3DenseVec3 x(m_mesh->vertexCount);
	b3DenseVec3 p(m_mesh->vertexCount);
	b3DenseVec3 v(m_mesh->vertexCount);
	b3DenseVec3 fe(m_mesh->vertexCount);
	b3DenseVec3 z(m_mesh->vertexCount);
	b3DiagMat33 S(m_mesh->vertexCount);

	for (u32 i = 0; i < m_mesh->vertexCount; ++i)
	{
		b3SoftBodyNode* n = m_nodes + i;

		M(i, i) = b3Diagonal(n->m_mass);
		
		// Rayleigh damping 
		// C = alpha * M + beta * K
		// Here the stiffness coefficient beta is zero
		C(i, i) = b3Diagonal(n->m_damping * n->m_mass);
		
		x[i] = m_mesh->vertices[i];
		p[i] = n->m_position;
		v[i] = n->m_velocity;
		fe[i] = n->m_force;
		z[i] = n->m_velocity;

		// Apply gravity
		if (n->m_type == e_dynamicSoftBodyNode)
		{
			fe[i] += n->m_mass * gravity;
			S[i].SetIdentity();
		}
		else
		{
			S[i].SetZero();
		}
	}

	// Element assembly
	b3SparseMat33 K(m_mesh->vertexCount);

	b3DenseVec3 f0(m_mesh->vertexCount);
	f0.SetZero();

	b3DenseVec3 f_plastic(m_mesh->vertexCount);
	f_plastic.SetZero();

	for (u32 ei = 0; ei < m_mesh->tetrahedronCount; ++ei)
	{
		b3SoftBodyMeshTetrahedron* mt = m_mesh->tetrahedrons + ei;
		b3SoftBodyElement* e = m_elements + ei;

		b3Mat33* Ke = e->K;

		float32* Be = e->B;
		float32* Pe = e->P;
		float32* epsilon_plastic = e->epsilon_plastic;

		u32 v1 = mt->v1;
		u32 v2 = mt->v2;
		u32 v3 = mt->v3;
		u32 v4 = mt->v4;

		b3Vec3 p1 = p[v1];
		b3Vec3 p2 = p[v2];
		b3Vec3 p3 = p[v3];
		b3Vec3 p4 = p[v4];

		b3Mat33 R;
		if (b3_enableStiffnessWarping)
		{
			b3Vec3 e1 = p2 - p1;
			b3Vec3 e2 = p3 - p1;
			b3Vec3 e3 = p4 - p1;

			b3Mat33 E(e1, e2, e3);

			b3Mat33 A = E * e->invE;

			b3ExtractRotation(R, e->q, A);
		}
		else
		{
			R.SetIdentity();
		}

		b3Mat33 RT = b3Transpose(R);

		u32 vs[4] = { v1, v2, v3, v4 };

		for (u32 i = 0; i < 4; ++i)
		{
			u32 vi = vs[i];

			for (u32 j = 0; j < 4; ++j)
			{
				u32 vj = vs[j];

				K(vi, vj) += R * Ke[i + 4 * j] * RT;
			}
		}

		// Elasticity
		b3Vec3 x1 = x[v1];
		b3Vec3 x2 = x[v2];
		b3Vec3 x3 = x[v3];
		b3Vec3 x4 = x[v4];

		b3Vec3 xs[4] = { x1, x2, x3, x4 };

		b3Vec3 f0s[4];

		for (u32 i = 0; i < 4; ++i)
		{
			f0s[i].SetZero();

			for (u32 j = 0; j < 4; ++j)
			{
				f0s[i] += R * Ke[i + 4 * j] * xs[j];
			}
		}

		f0[v1] += f0s[0];
		f0[v2] += f0s[1];
		f0[v3] += f0s[2];
		f0[v4] += f0s[3];

		// Plasticity
		b3Vec3 ps[4] = { p1, p2, p3, p4 };

		b3Vec3 RT_x_x0[4];
		for (u32 i = 0; i < 4; ++i)
		{
			RT_x_x0[i] = RT * ps[i] - xs[i];
		}

		// 6 x 1
		float32 epsilon_total[6];
		b3Mul(epsilon_total, Be, 6, 12, &RT_x_x0[0].x, 12, 1);

		// 6 x 1
		float32 epsilon_elastic[6];
		for (u32 i = 0; i < 6; ++i)
		{
			epsilon_elastic[i] = epsilon_total[i] - epsilon_plastic[i];
		}

		float32 len_epsilon_elastic = b3Length(epsilon_elastic, 6);
		if (len_epsilon_elastic > m_body->m_c_yield)
		{
			float32 amount = h * b3Min(m_body->m_c_creep, inv_h);
			for (u32 i = 0; i < 6; ++i)
			{
				epsilon_plastic[i] += amount * epsilon_elastic[i];
			}
		}

		float32 len_epsilon_plastic = b3Length(epsilon_plastic, 6);
		if (len_epsilon_plastic > m_body->m_c_max)
		{
			float32 scale = m_body->m_c_max / len_epsilon_plastic;
			for (u32 i = 0; i < 6; ++i)
			{
				epsilon_plastic[i] *= scale;
			}
		}

		b3Vec3 fs_plastic[4];
		b3Mul(&fs_plastic[0].x, Pe, 12, 6, epsilon_plastic, 6, 1);
		for (u32 i = 0; i < 4; ++i)
		{
			fs_plastic[i] = R * fs_plastic[i];
		}

		f_plastic[v1] += fs_plastic[0];
		f_plastic[v2] += fs_plastic[1];
		f_plastic[v3] += fs_plastic[2];
		f_plastic[v4] += fs_plastic[3];
	}

	f0 = -f0;

	b3SparseMat33 A = M + h * C + h * h * K;
	
	b3SparseMat33View viewA(A);

	b3DenseVec3 b = M * v - h * (K * p + f0 - (f_plastic + fe));

	// Solve Ax = b
	b3DenseVec3 sx(m_mesh->vertexCount);
	b3SolveMPCG(sx, viewA, b, z, S);

	// Solve constraints
	b3SoftBodyContactSolverDef contactSolverDef;
	contactSolverDef.allocator = m_allocator;
	contactSolverDef.positions = &p;
	contactSolverDef.velocities = &sx;
	contactSolverDef.bodyContactCapacity = m_mesh->vertexCount;

	b3SoftBodyContactSolver contactSolver(contactSolverDef);

	for (u32 i = 0; i < m_mesh->vertexCount; ++i)
	{
		if (m_nodes[i].m_bodyContact.active)
		{
			contactSolver.Add(&m_nodes[i].m_bodyContact);
		}
	}

	{
		contactSolver.InitializeBodyContactConstraints();
	}

	{
		contactSolver.WarmStart();
	}

	// Solve velocity constraints
	{
		for (u32 i = 0; i < velocityIterations; ++i)
		{
			contactSolver.SolveBodyContactVelocityConstraints();
		}
	}

	{
		contactSolver.StoreImpulses();
	}

	// Integrate velocities
	p = p + h * sx;

	// Solve position constraints
	{
		bool positionSolved = false;
		for (u32 i = 0; i < positionIterations; ++i)
		{
			bool bodyContactsSolved = contactSolver.SolveBodyContactPositionConstraints();

			if (bodyContactsSolved)
			{
				positionSolved = true;
				break;
			}
		}
	}

	// Synchronize bodies
	for (u32 i = 0; i < m_mesh->vertexCount; ++i)
	{
		b3SoftBodyNode* n = m_nodes + i;

		b3NodeBodyContact* c = &n->m_bodyContact;

		if (c->active == false)
		{
			continue;
		}

		b3Body* body = c->s2->GetBody();

		body->SynchronizeTransform();

		body->m_worldInvI = b3RotateToFrame(body->m_invI, body->m_xf.rotation);

		body->SynchronizeShapes();
	}

	// Copy state buffers back to the nodes
	for (u32 i = 0; i < m_mesh->vertexCount; ++i)
	{
		b3SoftBodyNode* n = m_nodes + i;

		n->m_position = p[i];
		n->m_velocity = sx[i];
	}
}