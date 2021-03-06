#include "src/models/Oil2d/Oil2dSolver.hpp"
#include <iostream>

#include "adolc/sparse/sparsedrivers.h"
#include "adolc/drivers/drivers.h"

using namespace oil2d;
using std::vector;
using std::ofstream;
using std::map;
using std::endl;
using std::setprecision;

Oil2dSolver::Oil2dSolver(Model* _model) : AbstractSolver<Model>(_model)
{
	y = new double[var_size * size];

	const int strNum = var_size * model->cellsNum;
	ind_i = new int[mesh::stencil * var_size * strNum];
	ind_j = new int[mesh::stencil * var_size * strNum];
	cols = new int[strNum];
	a = new double[mesh::stencil * var_size * strNum];
	ind_rhs = new int[strNum];
	rhs = new double[strNum];

	options[0] = 0;          /* sparsity pattern by index domains (default) */
	options[1] = 0;          /*                         safe mode (default) */
	options[2] = 0;          /*              not required if options[0] = 0 */
	options[3] = 0;          /*                column compression (default) */

	plot_P.open("snaps/P.dat", ofstream::out);
	plot_Q.open("snaps/Q.dat", ofstream::out);
};
Oil2dSolver::~Oil2dSolver()
{
	delete[] y;
	/*for (int i = 0; i < Model::var_size * size; i++)
		delete[] jac[i];
	delete[] jac;*/

	delete[] ind_i, ind_j, ind_rhs;
	delete[] cols;
	delete[] a, rhs;

	plot_P.close();
	plot_Q.close();
};
void Oil2dSolver::writeData()
{
	double p = 0.0, q = 0;

	plot_Q << cur_t * t_dim / 3600.0;

	map<size_t, double>::iterator it;
	for (it = model->Qcell.begin(); it != model->Qcell.end(); ++it)
	{
		p += model->u_next[it->first * Model::var_size] * model->P_dim;
		if (model->leftBoundIsRate)
			plot_Q << "\t" << it->second * model->Q_dim * 86400.0;
		else
		{
			plot_Q << "\t" << model->getRate(it->first) * model->Q_dim * 86400.0;
			q += model->getRate(it->first);
		}
	}
	plot_P << cur_t * t_dim / 3600.0 << "\t" << p / (double)(model->Qcell.size()) << endl;

	if (model->leftBoundIsRate)
		plot_Q << "\t" << model->Q_sum * model->Q_dim * 86400.0 << endl;
	else
		plot_Q << "\t" << q * model->Q_dim * 86400.0 << endl;
}
void Oil2dSolver::control()
{
	writeData();

	if (cur_t >= model->period[curTimePeriod])
	{
		curTimePeriod++;
		model->ht = model->ht_min;
		model->setPeriod(curTimePeriod);
	}

	if (model->ht <= model->ht_max && iterations < 6)
		model->ht = model->ht * 1.5;
	else if (iterations > 6 && model->ht > model->ht_min)
		model->ht = model->ht / 1.5;

	if (cur_t + model->ht > model->period[curTimePeriod])
		model->ht = model->period[curTimePeriod] - cur_t;

	cur_t += model->ht;
}
void Oil2dSolver::start()
{
	int counter = 0;
	iterations = 8;

	fillIndices();
	solver.Init(Model::var_size * model->cellsNum, 1.e-15, 1.e-15);

	model->setPeriod(curTimePeriod);
	while (cur_t < Tt)
	{
		control();
		model->snapshot_all(counter++);
		doNextStep();
		copyTimeLayer();
		cout << "---------------------NEW TIME STEP---------------------" << endl;
		cout << setprecision(6);
		cout << "time = " << cur_t << endl;
	}
	model->snapshot_all(counter++);
	writeData();
}
void Oil2dSolver::solveStep()
{
	int cellIdx, varIdx;
	double err_newton = 1.0;
	double averPrev = averValue(0), aver, dAver = 1.0;

	iterations = 0;
	while (err_newton > 1.e-4 /*&& (dAverSat > 1.e-9 || dAverPres > 1.e-7)*/ && iterations < 20)
	{
		copyIterLayer();

		computeJac();
		fill();
		solver.Assemble(ind_i, ind_j, a, elemNum, ind_rhs, rhs);
 		solver.Solve(PRECOND::ILU_SIMPLE);
		copySolution(solver.getSolution());

		//if (repeat == 0)
		//	repeat = 1;

		err_newton = convergance(cellIdx, varIdx);
		aver = averValue(0);		dAver = fabs(aver - averPrev);		averPrev = aver;
		iterations++;
	}

	cout << "Newton Iterations = " << iterations << endl;
}
void Oil2dSolver::copySolution(const paralution::LocalVector<double>& sol)
{
	for (int i = 0; i < size; i++)
	{
		auto& var = (*model)[i].u_next;
		var.p += sol[Model::var_size * i];
	}
}

void Oil2dSolver::computeJac()
{
	trace_on(0);

	for (size_t i = 0; i < size; i++)
		model->x[i].p <<= model->u_next[i * var_size];

	const int well_idx = model->cellsNum - 1;
	for (int i = 0; i < mesh->inner_cells; i++)
	{
		const auto& cell = mesh->cells[i];
		adouble isWellCell = (cell.type == CellType::WELL) ? true : false;
		condassign(model->h[i], isWellCell,
			(model->x[cell.id].p - model->x[well_idx].p) / model->P_dim,
			cell.V * model->solveInner(cell));
	}
	for (int i = mesh->border_beg; i < model->cellsNum - 1; i++)
	{
		const auto& cell = mesh->cells[i];
		model->h[i] = model->solveBorder(cell);
	}
	
	adouble leftIsRate = model->leftBoundIsRate;
	adouble tmp = model->solveWell(mesh->cells[well_idx]);
	condassign(model->h[well_idx], leftIsRate,
		tmp + model->ht * model->props_oil.getDensity(model->x[well_idx].p) * model->Q_sum,
		(model->x[well_idx].p - model->Pwf) / model->P_dim);

	for (int i = 0; i < Model::var_size * size; i++)
		model->h[i] >>= y[i];

	trace_off();
}
void Oil2dSolver::fill()
{
	sparse_jac(0, Model::var_size * model->cellsNum, Model::var_size * model->cellsNum, repeat,
		&model->u_next[0], &elemNum, (unsigned int**)(&ind_i), (unsigned int**)(&ind_j), &a, options);

	int counter = 0;
	for (const auto& cell : mesh->cells)
	{
		//getMatrixStencil(cell);
		for (int i = 0; i < Model::var_size; i++)
		{
			const int str_idx = Model::var_size * cell.id + i;
			/*for (const int idx : stencil_idx)
			{
				for (int j = 0; j < Model::var_size; j++)
					a[counter++] = jac[str_idx][Model::var_size * idx + j];
			}*/

			rhs[str_idx] = -y[str_idx];
		}
		//stencil_idx.clear();
	}
}