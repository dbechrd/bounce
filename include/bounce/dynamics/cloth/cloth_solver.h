/*
* Copyright (c) 2016-2016 Irlan Robson http://www.irlan.net
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

#ifndef B3_CLOTH_SOLVER_H
#define B3_CLOTH_SOLVER_H

#include <bounce/common/math/vec3.h>
#include <bounce/common/math/mat33.h>

struct b3DenseVec3;
struct b3DiagMat33;
struct b3SparseMat33;

struct b3Particle;
struct b3Spring;
struct b3ParticleContact;

class b3Shape;
class b3StackAllocator;

struct b3ClothSolverDef
{
	b3StackAllocator* stack;
	u32 particleCapacity;
	u32 springCapacity;
	u32 contactCapacity;
};

class b3ClothSolver
{
public:
	b3ClothSolver(const b3ClothSolverDef& def);
	~b3ClothSolver();
	
	void Add(b3Particle* p);
	void Add(b3Spring* s);
	void Add(b3ParticleContact* c);

	void Solve(float32 dt, const b3Vec3& gravity);
private:
	// Compute forces.
	void Compute_f(b3DenseVec3& f, const b3DenseVec3& x, const b3DenseVec3& v, const b3Vec3& gravity);
	
	// Compute A and b in Ax = b
	void Compute_A_b(b3SparseMat33& A, b3DenseVec3& b, const b3DenseVec3& f, const b3DenseVec3& x, const b3DenseVec3& v, const b3DenseVec3& y) const;

	// Compute S.
	void Compute_S(b3DiagMat33& S);

	// Compute z. 
	void Compute_z(b3DenseVec3& z);

	// Solve Ax = b.
	// Output x and the residual error f = Ax - b ~ 0.
	void Solve(b3DenseVec3& x, u32& iterations, const b3SparseMat33& A, const b3DenseVec3& b, const b3DiagMat33& S, const b3DenseVec3& z, const b3DenseVec3& y) const;

	b3StackAllocator* m_allocator;

	float32 m_h;
	
	u32 m_particleCapacity;
	u32 m_particleCount;
	b3Particle** m_particles;

	u32 m_springCapacity;
	u32 m_springCount;
	b3Spring** m_springs;
	b3Mat33* m_Jx;
	b3Mat33* m_Jv;

	u32 m_contactCapacity;
	u32 m_contactCount;
	b3ParticleContact** m_contacts;
};

#endif