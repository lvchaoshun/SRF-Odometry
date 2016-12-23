/* Project: Laser odometry
   Author: Mariano Jaimez Tarifa
   Date: January 2015 */

#include "laser_odometry_v1.h"


using namespace mrpt::utils;
using namespace Eigen;
using namespace std;


void RF2O::initialize(unsigned int size, float FOV_rad, bool is_test)
{
    test = is_test;
    cols = size;
    width = size;
    fovh = FOV_rad;
    ctf_levels = ceilf(log2(cols) - 4.3f);
	
    //Resize original range scan
    range_wf.resize(width);

    //Resize the transformation matrix
    transformations.resize(ctf_levels);
    for (unsigned int i = 0; i < ctf_levels; i++)
        transformations[i].resize(3,3);

	//Resize pyramid
	unsigned int s, cols_i;
    const unsigned int pyr_levels = round(log2(round(float(width)/float(cols)))) + ctf_levels;
    range.resize(pyr_levels);
    range_old.resize(pyr_levels);
    range_inter.resize(pyr_levels);
    xx.resize(pyr_levels);
    xx_inter.resize(pyr_levels);
    xx_old.resize(pyr_levels);
    yy.resize(pyr_levels);
    yy_inter.resize(pyr_levels);
    yy_old.resize(pyr_levels);
	range_warped.resize(pyr_levels);
	xx_warped.resize(pyr_levels);
	yy_warped.resize(pyr_levels);

	for (unsigned int i = 0; i<pyr_levels; i++)
    {
        s = pow(2.f,int(i));
        cols_i = ceil(float(width)/float(s));

        range[i].resize(cols_i); range_inter[i].resize(cols_i); range_old[i].resize(cols_i);
        range[i].fill(0.f); range_old[i].fill(0.f);
        xx[i].resize(cols_i); xx_inter[i].resize(cols_i); xx_old[i].resize(cols_i);
        xx[i].fill(0.f); xx_old[i].fill(0.f);
        yy[i].resize(cols_i); yy_inter[i].resize(cols_i); yy_old[i].resize(cols_i);
        yy[i].fill(0.f); yy_old[i].fill(0.f);

		if (cols_i <= cols)
		{
            range_warped[i].resize(cols_i);
            xx_warped[i].resize(cols_i);
            yy_warped[i].resize(cols_i);
		}
    }

    //Resize aux variables
    dt.resize(cols);
    dtita.resize(cols);
    weights.resize(cols);
    null.resize(cols);
    null.fill(false);
	cov_odo.assign(0.f);
    outliers.resize(cols);
    outliers.fill(false);


	//Compute gaussian mask
	g_mask[0] = 1.f/16.f; g_mask[1] = 0.25f; g_mask[2] = 6.f/16.f; g_mask[3] = g_mask[1]; g_mask[4] = g_mask[0];

    //Initialize "last velocity" as zero
	kai_abs.assign(0.f);
	kai_loc_old.assign(0.f);
}


void RF2O::createScanPyramid()
{
	const float max_range_dif = 0.3f;
	
	//Push the frames back
	range_old.swap(range);
	xx_old.swap(xx);
	yy_old.swap(yy);

    //The number of levels of the pyramid does not match the number of levels used
    //in the odometry computation (because we sometimes want to finish with lower resolutions)

    unsigned int pyr_levels = round(log2(round(float(width)/float(cols)))) + ctf_levels;

    //Generate levels
    for (unsigned int i = 0; i<pyr_levels; i++)
    {
        unsigned int s = pow(2.f,int(i));
        cols_i = ceil(float(width)/float(s));
		const unsigned int i_1 = i-1;

        //              First level -> Filter, not downsample
        //------------------------------------------------------------------------
        if (i == 0)
		{
			for (unsigned int u = 0; u < cols_i; u++)
            {	
				const float dcenter = range_wf(u);
					
				//Inner pixels
                if ((u>1)&&(u<cols_i-2))
                {		
					if (dcenter > 0.f)
					{	
                        float sum = 0.f, weight = 0.f;

						for (int l=-2; l<3; l++)
						{
							const float abs_dif = abs(range_wf(u+l)-dcenter);
							if (abs_dif < max_range_dif)
							{
								const float aux_w = g_mask[2+l]*(max_range_dif - abs_dif);
								weight += aux_w;
								sum += aux_w*range_wf(u+l);
							}
						}
						range[i](u) = sum/weight;
					}
					else
						range[i](u) = 0.f;

                }

                //Boundary
                else
                {
                    if (dcenter > 0.f)
					{						
                        float sum = 0.f, weight = 0.f;

						for (int l=-2; l<3; l++)	
						{
							const int indu = u+l;
							if ((indu>=0)&&(indu<cols_i))
							{
								const float abs_dif = abs(range_wf(indu)-dcenter);										
								if (abs_dif < max_range_dif)
								{
									const float aux_w = g_mask[2+l]*(max_range_dif - abs_dif);
									weight += aux_w;
									sum += aux_w*range_wf(indu);
								}
							}
						}
						range[i](u) = sum/weight;
					}
					else
						range[i](u) = 0.f;

                }
            }
		}

        //                              Downsampling
        //-----------------------------------------------------------------------------
        else
        {            
			for (unsigned int u = 0; u < cols_i; u++)
            {
                const int u2 = 2*u;		
				const float dcenter = range[i_1](u2);
					
				//Inner pixels
                if ((u>0)&&(u<cols_i-1))
                {		
					if (dcenter > 0.f)
					{	
                        float sum = 0.f, weight = 0.f;

						for (int l=-2; l<3; l++)
						{
							const float abs_dif = abs(range[i_1](u2+l)-dcenter);
							if (abs_dif < max_range_dif)
							{
								const float aux_w = g_mask[2+l]*(max_range_dif - abs_dif);
								weight += aux_w;
								sum += aux_w*range[i_1](u2+l);
							}
						}
						range[i](u) = sum/weight;
					}
					else
						range[i](u) = 0.f;

                }

                //Boundary
                else
                {
                    if (dcenter > 0.f)
					{						
                        float sum = 0.f, weight = 0.f;
                        const unsigned int cols_i2 = range[i_1].rows();

						for (int l=-2; l<3; l++)	
						{
							const int indu = u2+l;

							if ((indu>=0)&&(indu<cols_i2))
							{
								const float abs_dif = abs(range[i_1](indu)-dcenter);										
								if (abs_dif < max_range_dif)
								{
									const float aux_w = g_mask[2+l]*(max_range_dif - abs_dif);
									weight += aux_w;
									sum += aux_w*range[i_1](indu);
								}
							}
						}
						range[i](u) = sum/weight;

					}
					else
						range[i](u) = 0.f;
                }
            }
        }

        //Calculate coordinates "xy" of the points
        for (unsigned int u = 0; u < cols_i; u++) 
		{
            if (range[i](u) > 0.f)
			{
                const float tita = -0.5f*fovh + float(u)*fovh/float(cols_i-1);
				xx[i](u) = range[i](u)*cos(tita);
				yy[i](u) = range[i](u)*sin(tita);
			}
			else
			{
				xx[i](u) = 0.f;
				yy[i](u) = 0.f;
			}
		}
    }
}

void RF2O::calculateCoord()
{		
    null.fill(false);
    num_valid_range = 0;

    for (unsigned int u = 0; u < cols_i; u++)
	{
		if ((range_old[image_level](u) == 0.f) || (range_warped[image_level](u) == 0.f))
		{
			range_inter[image_level](u) = 0.f;
			xx_inter[image_level](u) = 0.f;
			yy_inter[image_level](u) = 0.f;
            null(u) = true;
		}
		else
		{
			range_inter[image_level](u) = 0.5f*(range_old[image_level](u) + range_warped[image_level](u));
			xx_inter[image_level](u) = 0.5f*(xx_old[image_level](u) + xx_warped[image_level](u));
			yy_inter[image_level](u) = 0.5f*(yy_old[image_level](u) + yy_warped[image_level](u));
            null(u) = false;
            if ((u>0)&&(u<cols_i-1))
                num_valid_range++;
		}
	}
}

void RF2O::calculaterangeDerivativesSurface()
{	
    //Compute distances between points
    rtita.resize(cols_i);
    rtita.fill(1.f);

	for (unsigned int u = 0; u < cols_i-1; u++)
    {
		const float dist = square(xx_inter[image_level](u+1) - xx_inter[image_level](u))
							+ square(yy_inter[image_level](u+1) - yy_inter[image_level](u));
		if (dist  > 0.f)
            rtita(u) = sqrtf(dist);
	}

    //Spatial derivatives
    for (unsigned int u = 1; u < cols_i-1; u++)
		dtita(u) = (rtita(u-1)*(range_inter[image_level](u+1)-range_inter[image_level](u)) + rtita(u)*(range_inter[image_level](u) - range_inter[image_level](u-1)))/(rtita(u)+rtita(u-1));

	dtita(0) = dtita(1);
	dtita(cols_i-1) = dtita(cols_i-2);

	//Temporal derivative
	for (unsigned int u = 0; u < cols_i; u++)
        dt(u) = range_warped[image_level](u) - range_old[image_level](u);

}


void RF2O::computeWeights()
{
	//The maximum weight size is reserved at the constructor
    weights.fill(0.f);
	
	//Parameters for error_linearization
	const float kdtita = 1.f;
    const float kdt = kdtita;
	const float k2d = 0.2f;
    const float sensor_sigma = 5.f*4e-4f;
    //const float sensor_sigma = test ? 4e-4f : 10e-8f;
	
	for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
		{	
			//							Compute derivatives
			//-----------------------------------------------------------------------
			const float ini_dtita = range_old[image_level](u+1) - range_old[image_level](u-1);
			const float final_dtita = range_warped[image_level](u+1) - range_warped[image_level](u-1);

			const float dtitat = ini_dtita - final_dtita;
			const float dtita2 = dtita(u+1) - dtita(u-1);

            const float w_der = kdt*square(dt(u)) + kdtita*square(dtita(u)) + k2d*(abs(dtitat) + abs(dtita2)) + sensor_sigma; //It could be function of the range as well

            weights(u) = sqrtf(1.f/w_der);
		}

    const float inv_max = 1.f/weights.maxCoeff();
	weights = inv_max*weights;
}


void RF2O::solveSystemQuadResiduals()
{
	A.resize(num_valid_range,3);
    B.resize(num_valid_range);
	unsigned int cont = 0;
	const float kdtita = (cols_i-1)/fovh;

	//Fill the matrix A and the vector B
	//The order of the variables will be (vx, vy, wz)

	for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
		{
			// Precomputed expressions
			const float tw = weights(u);
            const float tita = -0.5f*fovh + u/kdtita;

			//Fill the matrix A
			A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
			A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            if (test)
                A(cont, 2) = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);
            else
                A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

			cont++;
		}
	
	//Solve the linear system of equations using a minimum least squares method
	MatrixXf AtA, AtB;
	AtA.multiply_AtA(A);
	AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);

    //Covariance matrix calculation
    VectorXf res = A*kai_loc_level - B;
	cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();
}

void RF2O::solveSystemQuadResidualsNoPreW()
{
    A.resize(num_valid_range,3);
    B.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = (cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = 1.f; //weights(u);
            const float tita = -0.5f*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    AtA.multiply_AtA(A);
    AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);

    //Covariance matrix calculation
    VectorXf res = A*kai_loc_level - B;
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();
}


void RF2O::solveSystemMCauchy()
{
	A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
	unsigned int cont = 0;
	const float kdtita = float(cols_i-1)/fovh;

	//Fill the matrix A and the vector B
	//The order of the variables will be (vx, vy, wz)

	for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
		{
			// Precomputed expressions
			const float tw = weights(u);
			const float tita = -0.5*fovh + u/kdtita;

			//Fill the matrix A
			A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
			A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

			cont++;
		}
	
	//Solve the linear system of equations using a minimum least squares method
	MatrixXf AtA, AtB;
	AtA.multiply_AtA(A);
	AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);
    VectorXf res = A*kai_loc_level - B;
	//cout << endl << "max res: " << res.maxCoeff();
	//cout << endl << "min res: " << res.minCoeff();

    //Compute the average dt and res
    float aver_dt = 0.f, aver_res = 0.f; unsigned int ind = 0;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            aver_dt += fabsf(dt(u));
            aver_res += fabsf(res(ind++));
        }
    aver_dt /= cont; aver_res /= cont;
    const float k = 10.f/aver_dt; //200

    ////Compute the energy
    //float energy = 0.f;
    //for (unsigned int i=0; i<res.rows(); i++)
    //	energy += log(1.f + square(k*res(i)));
    //printf("\n\nEnergy(0) = %f", energy);

    //Solve iterative reweighted least squares
    //===================================================================
    for (unsigned int i=1; i<=iter_irls; i++)
    {
        cont = 0;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                const float res_weight = sqrtf(1.f/(1.f + square(k*res(cont))));

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        ////Compute the energy
        //energy = 0.f;
        //for (unsigned int j=0; j<res.rows(); j++)
        //	energy += log(1.f + square(k*res(j)));
        //printf("\nEnergy(%d) = %f", i, energy);
    }

    //Covariance calculation
	cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();
}

void RF2O::solveSystemMTukey()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = weights(u);
            const float tita = -0.5*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    AtA.multiply_AtA(A);
    AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);
    VectorXf res = A*kai_loc_level - B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 5.f*mad;

    ////Compute the energy
    //float energy = 0.f;
    //for (unsigned int i=0; i<res.rows(); i++)
    //	energy += log(1.f + square(k*res(i)));
    //printf("\n\nEnergy(0) = %f", energy);

    //Solve iterative reweighted least squares
    //===================================================================
    for (unsigned int i=1; i<=iter_irls; i++)
    {
        cont = 0;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = square(1.f - square(res(cont)/c));
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        ////Compute the energy
        //energy = 0.f;
        //for (unsigned int j=0; j<res.rows(); j++)
        //	energy += log(1.f + square(k*res(j)));
        //printf("\nEnergy(%d) = %f", i, energy);

        //Recompute c
        //-------------------------------------------------
        //Compute the median of res
        aux_vector.clear();
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(res(k));
        std::sort(aux_vector.begin(), aux_vector.end());
        res_median = aux_vector.at(res.rows()/2);

        //Compute the median absolute deviation
        aux_vector.clear();
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(abs(res(k) - res_median));
        std::sort(aux_vector.begin(), aux_vector.end());
        mad = aux_vector.at(res.rows()/2);

        //Find the m-estimator constant
        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

    //Update the outlier mask
    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
            if (abs(res(cont++)) > c)
            {
                outliers(u) = true;
                num_outliers++;
            }

    printf("\n Num_outliers = %d", num_outliers);
}

void RF2O::solveSystemTruncatedQuad()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = weights(u);
            const float tita = -0.5*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    AtA.multiply_AtA(A);
    AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);
    VectorXf res = A*kai_loc_level - B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 5.f*mad;

    ////Compute the energy
    //float energy = 0.f;
    //for (unsigned int i=0; i<res.rows(); i++)
    //	energy += log(1.f + square(k*res(i)));
    //printf("\n\nEnergy(0) = %f", energy);

    //Solve iterative reweighted least squares
    //===================================================================
    for (unsigned int i=1; i<=iter_irls; i++)
    {
        cont = 0;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = 1.f;
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        ////Compute the energy
        //energy = 0.f;
        //for (unsigned int j=0; j<res.rows(); j++)
        //	energy += log(1.f + square(k*res(j)));
        //printf("\nEnergy(%d) = %f", i, energy);

        //Recompute c
        //-------------------------------------------------
        //Compute the median of res
        aux_vector.clear();
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(res(k));
        std::sort(aux_vector.begin(), aux_vector.end());
        res_median = aux_vector.at(res.rows()/2);

        //Compute the median absolute deviation
        aux_vector.clear();
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(abs(res(k) - res_median));
        std::sort(aux_vector.begin(), aux_vector.end());
        mad = aux_vector.at(res.rows()/2);

        //Find the m-estimator constant
        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

    //Update the outlier mask
    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
            if (abs(res(cont++)) > c)
            {
                outliers(u) = true;
                num_outliers++;
            }

    printf("\n Num_outliers = %d", num_outliers);
}

void RF2O::solveSystemSmoothTruncQuad()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = weights(u);
            const float tita = -0.5f*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    AtA.multiply_AtA(A);
    AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);
    VectorXf res = A*kai_loc_level - B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 4.f*mad; //This seems to be the best (4)

    //Compute the energy
    float new_energy = 0.f, last_energy;
    for (unsigned int i=0; i<res.rows(); i++)
    {
        if (abs(res(i)) < c)     new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
        else                     new_energy += 0.25f*square(c);
    }
    //printf("\n\nEnergy(0) = %f", new_energy);
    last_energy = 2.f*new_energy;
    unsigned int iter = 1;

    //Solve iteratively reweighted least squares
    //===================================================================
    while ((new_energy < 0.995f*last_energy)&&(iter < 10))
    {
        cont = 0;
        last_energy = new_energy;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = (1.f - square(res(cont)/c));
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        //Compute the energy
        new_energy = 0.f;
        for (unsigned int i=0; i<res.rows(); i++)
        {
            if (abs(res(i)) < c)    new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
            else                    new_energy += 0.25f*square(c);
        }
        //printf("\nEnergy(%d) = %f", iter, new_energy);
        iter++;

//        //Recompute c
//        //-------------------------------------------------
//        //Compute the median of res
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(res(k));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        res_median = aux_vector.at(res.rows()/2);

//        //Compute the median absolute deviation
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(abs(res(k) - res_median));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        mad = aux_vector.at(res.rows()/2);

//        //Find the m-estimator constant
//        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

    //Update the outlier mask
//    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
//    for (unsigned int u = 1; u < cols_i-1; u++)
//        if (null(u) == false)
//            if (abs(res(cont++)) > c)
//            {
//                outliers(u) = true;
//                num_outliers++;
//            }

//    printf("\n Num_outliers = %d", num_outliers);
}

void RF2O::solveSystemSmoothTruncQuadNoPreW()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = 1.f; //weights(u);
            const float tita = -0.5f*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    AtA.multiply_AtA(A);
    AtB.multiply_AtB(A,B);
    kai_loc_level = AtA.ldlt().solve(AtB);
    VectorXf res = A*kai_loc_level - B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 4.f*mad; //This seems to be the best (4)

    //Compute the energy
    float new_energy = 0.f, last_energy;
    for (unsigned int i=0; i<res.rows(); i++)
    {
        if (abs(res(i)) < c)     new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
        else                     new_energy += 0.25f*square(c);
    }
    //printf("\n\nEnergy(0) = %f", new_energy);
    last_energy = 2.f*new_energy;
    unsigned int iter = 1;

    //Solve iteratively reweighted least squares
    //===================================================================
    while ((new_energy < 0.995f*last_energy)&&(iter < 10))
    {
        cont = 0;
        last_energy = new_energy;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = (1.f - square(res(cont)/c));
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        //Compute the energy
        new_energy = 0.f;
        for (unsigned int i=0; i<res.rows(); i++)
        {
            if (abs(res(i)) < c)    new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
            else                    new_energy += 0.25f*square(c);
        }
        //printf("\nEnergy(%d) = %f", iter, new_energy);
        iter++;

//        //Recompute c
//        //-------------------------------------------------
//        //Compute the median of res
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(res(k));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        res_median = aux_vector.at(res.rows()/2);

//        //Compute the median absolute deviation
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(abs(res(k) - res_median));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        mad = aux_vector.at(res.rows()/2);

//        //Find the m-estimator constant
//        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

    //Update the outlier mask
//    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
//    for (unsigned int u = 1; u < cols_i-1; u++)
//        if (null(u) == false)
//            if (abs(res(cont++)) > c)
//            {
//                outliers(u) = true;
//                num_outliers++;
//            }

//    printf("\n Num_outliers = %d", num_outliers);
}

void RF2O::solveSystemSmoothTruncQuadNoPreW2()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = 1.f; //weights(u);
            const float tita = -0.5f*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Solve the linear system of equations using a minimum least squares method
    MatrixXf AtA, AtB;
    VectorXf res = -B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 4.f*mad; //This seems to be the best (4)

    //Compute the energy
    float new_energy = 0.f, last_energy;
    for (unsigned int i=0; i<res.rows(); i++)
    {
        if (abs(res(i)) < c)     new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
        else                     new_energy += 0.25f*square(c);
    }
    //printf("\n\nEnergy(0) = %f", new_energy);
    last_energy = 2.f*new_energy + 1.f;
    unsigned int iter = 1;

    //Solve iteratively reweighted least squares
    //===================================================================
    while ((new_energy < 0.995f*last_energy)&&(iter < 10))
    {
        cont = 0;
        last_energy = new_energy;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = (1.f - square(res(cont)/c));
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        //Compute the energy
        new_energy = 0.f;
        for (unsigned int i=0; i<res.rows(); i++)
        {
            if (abs(res(i)) < c)    new_energy += 0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/c));
            else                    new_energy += 0.25f*square(c);
        }
        //printf("\nEnergy(%d) = %f", iter, new_energy);
        iter++;

//        //Recompute c
//        //-------------------------------------------------
//        //Compute the median of res
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(res(k));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        res_median = aux_vector.at(res.rows()/2);

//        //Compute the median absolute deviation
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(abs(res(k) - res_median));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        mad = aux_vector.at(res.rows()/2);

//        //Find the m-estimator constant
//        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();
}

void RF2O::solveSystemSmoothTruncQuadFromBeginning()
{
    A.resize(num_valid_range,3); Aw.resize(num_valid_range,3);
    B.resize(num_valid_range); Bw.resize(num_valid_range);
    unsigned int cont = 0;
    const float kdtita = float(cols_i-1)/fovh;

    //Fill the matrix A and the vector B
    //The order of the variables will be (vx, vy, wz)

    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = weights(u);
            const float tita = -0.5*fovh + u/kdtita;

            //Fill the matrix A
            A(cont, 0) = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            A(cont, 1) = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            A(cont, 2) = tw*(-yy_inter[image_level](u)*cos(tita) + xx_inter[image_level](u)*sin(tita) - dtita(u)*kdtita);
            B(cont) = tw*(-dt(u));

            cont++;
        }

    //Create variables and compute initial residuals
    MatrixXf AtA, AtB;
    VectorXf res = B;
    //cout << endl << "max res: " << res.maxCoeff();
    //cout << endl << "min res: " << res.minCoeff();

    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find the m-estimator constant
    float c = 5.f*mad;

    ////Compute the energy
    //float energy = 0.f;
    //for (unsigned int i=0; i<res.rows(); i++)
    //	energy += log(1.f + square(k*res(i)));
    //printf("\n\nEnergy(0) = %f", energy);

    //Solve iterative reweighted least squares
    //===================================================================
    for (unsigned int i=0; i<=iter_irls; i++)
    {
        cont = 0;

        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                float res_weight;
                if (abs(res(cont)) <= c)    res_weight = (1.f - square(res(cont)/c));
                else                        res_weight = 0.f;

                //Fill the matrix Aw
                Aw(cont,0) = res_weight*A(cont,0);
                Aw(cont,1) = res_weight*A(cont,1);
                Aw(cont,2) = res_weight*A(cont,2);
                Bw(cont) = res_weight*B(cont);
                cont++;
            }

        //Solve the linear system of equations using a minimum least squares method
        AtA.multiply_AtA(Aw);
        AtB.multiply_AtB(Aw,Bw);
        kai_loc_level = AtA.ldlt().solve(AtB);
        res = A*kai_loc_level - B;

        ////Compute the energy
        //energy = 0.f;
        //for (unsigned int j=0; j<res.rows(); j++)
        //	energy += log(1.f + square(k*res(j)));
        //printf("\nEnergy(%d) = %f", i, energy);

//        //Recompute c
//        //-------------------------------------------------
//        //Compute the median of res
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(res(k));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        res_median = aux_vector.at(res.rows()/2);

//        //Compute the median absolute deviation
//        aux_vector.clear();
//        for (unsigned int k = 0; k<res.rows(); k++)
//            aux_vector.push_back(abs(res(k) - res_median));
//        std::sort(aux_vector.begin(), aux_vector.end());
//        mad = aux_vector.at(res.rows()/2);

//        //Find the m-estimator constant
//        c = 5.f*mad;

    }

    //Covariance calculation
    cov_odo = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

//    //Update the outlier mask
//    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
//    for (unsigned int u = 1; u < cols_i-1; u++)
//        if (null(u) == false)
//            if (abs(res(cont++)) > c)
//            {
//                outliers(u) = true;
//                num_outliers++;
//            }

//    printf("\n Num_outliers = %d", num_outliers);
}



void RF2O::solveLiftedSmoothTruncQuad()
{
    //Create/resize variables for the LM solver
    MatrixXf J(2*num_valid_range,3+num_valid_range); J.fill(0.f);
    VectorXf b(2*num_valid_range);
    VectorXf res(num_valid_range), w(num_valid_range);
    kai_loc_level.fill(0.f);
    float lambda = 0.001f;
    unsigned int cont = 0;

    solveSystemQuadResiduals();

    //Order: D1, R1, D2, R2, D3, R3...
    //Compute the initial residuals
    const float kdtita = float(cols_i-1)/fovh;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            // Precomputed expressions
            const float tw = weights(u);
            const float tita = -0.5f*fovh + u/kdtita;
            const float J_xi_0 = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
            const float J_xi_1 = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
            const float J_xi_2 = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);

            res(cont++) = J_xi_0*kai_loc_level(0) + J_xi_1*kai_loc_level(1) + J_xi_2*kai_loc_level(2) - tw*dt(u);
            //res(cont++) = -weights(u)*dt(u);
        }


    //Compute the median of res
    vector<float> aux_vector;
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(res(k));
    std::sort(aux_vector.begin(), aux_vector.end());
    float res_median = aux_vector.at(res.rows()/2);

    //Compute the median absolute deviation
    aux_vector.clear();
    for (unsigned int k = 0; k<res.rows(); k++)
        aux_vector.push_back(abs(res(k) - res_median));
    std::sort(aux_vector.begin(), aux_vector.end());
    float mad = aux_vector.at(res.rows()/2);

    //Find tau
    float tau = 5.f*mad;


    //Compute w - Do it properly with an if
    cont = 0;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
        {
            if (abs(res(cont)) <= tau)    w(cont) = 1.f - square(res(cont)/tau);
            else                          w(cont) = 0.f;
            cont++;
        }

    //printf("\n tau = %f, w_max = %f, w_min = %f, max_res = %f, min_res = %f", tau, w.maxCoeff(), w.minCoeff(), res.maxCoeff(), res.minCoeff());

    //Compute the initial energy
    float energy = 0.f, energy_lifted = 0.f;
    for (unsigned int i=0; i<res.rows(); i++)
    {
        if (abs(res(i)) < tau)   energy += 10000.f*0.5f*square(res(i))*(1.f - 0.5f*square(res(i)/tau));
        else                     energy += 10000.f*0.25f*square(tau);
        energy_lifted += 10000.f*0.5f*(square(w(i)*res(i)) + 0.5f*square(tau*(square(w(i)) - 1.f)));
    }
    printf("\n (initial) Energy = %f, Energy_lifted = %f", energy, energy_lifted);


    //Solve the lifted energy
    //----------------------------------------------------------
    for (unsigned int iter=1; iter<=15; iter++)
    {
        //Compute the Jacobian and the independent term
        cont = 0;
        const float kdtita = float(cols_i-1)/fovh;
        for (unsigned int u = 1; u < cols_i-1; u++)
            if (null(u) == false)
            {
                // Precomputed expressions
                const float tw = weights(u);
                const float tita = -0.5f*fovh + u/kdtita;
                const float J_xi_0 = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
                const float J_xi_1 = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
                const float J_xi_2 = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);

                res(cont) = J_xi_0*kai_loc_level(0) + J_xi_1*kai_loc_level(1) + J_xi_2*kai_loc_level(2) - tw*dt(u);

                //Di
                J(2*cont, 0) = w(cont)*J_xi_0;
                J(2*cont, 1) = w(cont)*J_xi_1;
                J(2*cont, 2) = w(cont)*J_xi_2;
                J(2*cont, 3 + cont) = res(cont);
                b(2*cont) = w(cont)*res(cont);

                //Ri
                J(2*cont+1, 3 + cont) = tau*sqrtf(2.f)*w(cont);
                b(2*cont+1) = tau*0.5f*sqrtf(2.f)*(square(w(cont)) - 1.f);
                cont++;
            }


        //Solve the linear system of equations using a minimum least squares method
        MatrixXf JtJ, Jtb;
        JtJ.multiply_AtA(J);
        Jtb.multiply_AtB(J,b);
        Jtb = -Jtb;

        bool energy_increasing = true;

        while (energy_increasing)
        {

            MatrixXf JtJ_lm = JtJ;
            JtJ_lm.diagonal() += lambda*JtJ_lm.diagonal();

            VectorXf incr = JtJ_lm.ldlt().solve(Jtb);
            VectorXf new_kai = kai_loc_level + incr.topRows(3);
            VectorXf new_w = w + incr.bottomRows(num_valid_range);

            //printf("\n Incr sum = %f, b sum = %f, Jtb sum = %f", incr.sumAll(), b.sumAll(), Jtb.sumAll());


            //Compute the energy
            energy = 0.f; energy_lifted = 0.f; cont = 0;
            const float kdtita = float(cols_i-1)/fovh;
            for (unsigned int u = 1; u < cols_i-1; u++)
                if (null(u) == false)
                {
                    // Precomputed expressions
                    const float tw = weights(u);
                    const float tita = -0.5f*fovh + u/kdtita;
                    const float J_xi_0 = tw*(cos(tita) + dtita(u)*kdtita*sin(tita)/range_inter[image_level](u));
                    const float J_xi_1 = tw*(sin(tita) - dtita(u)*kdtita*cos(tita)/range_inter[image_level](u));
                    const float J_xi_2 = tw*(-yy[image_level](u)*cos(tita) + xx[image_level](u)*sin(tita) - dtita(u)*kdtita);

                    res(cont) = J_xi_0*new_kai(0) + J_xi_1*new_kai(1) + J_xi_2*new_kai(2) - tw*dt(u);

                    if (abs(res(cont)) < tau)   energy += 10000.f*0.5f*square(res(cont))*(1.f - 0.5f*square(res(cont)/tau));
                    else                        energy += 10000.f*0.25f*square(tau);
                    energy_lifted += 10000.f*0.5f*(square(new_w(cont)*res(cont)) + 0.5f*square(tau*(square(new_w(cont)) - 1.f)));

                    cont++;
                }

            //Update variables
            kai_loc_level = new_kai;
            w = new_w;
            energy_increasing = false;

        }

        //Compute the median of res
        vector<float> aux_vector;
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(res(k));
        std::sort(aux_vector.begin(), aux_vector.end());
        float res_median = aux_vector.at(res.rows()/2);

        //Compute the median absolute deviation
        aux_vector.clear();
        for (unsigned int k = 0; k<res.rows(); k++)
            aux_vector.push_back(abs(res(k) - res_median));
        std::sort(aux_vector.begin(), aux_vector.end());
        float mad = aux_vector.at(res.rows()/2);

        //Find tau
        tau = 5.f*mad;


        printf("\n (iteration = %d) Energy = %f, Energy_lifted = %f", iter, energy, energy_lifted);

    }

    kai_loc_level = -kai_loc_level;

    //Covariance calculation
    cov_odo.fill(0.f); // = (1.f/float(num_valid_range-3))*AtA.inverse()*res.squaredNorm();

    //Update the outlier mask
    cont = 0; outliers.fill(false); unsigned int num_outliers = 0;
    for (unsigned int u = 1; u < cols_i-1; u++)
        if (null(u) == false)
            if (abs(res(cont++)) > tau)
            {
                outliers(u) = true;
                num_outliers++;
            }

    printf("\n Num_outliers = %d", num_outliers);
}

void RF2O::performWarping()
{
	Matrix3f acu_trans; 
	acu_trans.setIdentity();
	for (unsigned int i=1; i<=level; i++)
		acu_trans = transformations[i-1]*acu_trans;

    ArrayXf wacu(cols_i);
    wacu.fill(0.f);
    range_warped[image_level].fill(0.f);

	const float cols_lim = float(cols_i-1);
	const float kdtita = cols_lim/fovh;

	for (unsigned int j = 0; j<cols_i; j++)
	{				
		if (range[image_level](j) > 0.f)
		{
			//Transform point to the warped reference frame
			const float x_w = acu_trans(0,0)*xx[image_level](j) + acu_trans(0,1)*yy[image_level](j) + acu_trans(0,2);
			const float y_w = acu_trans(1,0)*xx[image_level](j) + acu_trans(1,1)*yy[image_level](j) + acu_trans(1,2);
			const float tita_w = atan2(y_w, x_w);
			const float range_w = sqrt(x_w*x_w + y_w*y_w);

			//Calculate warping
			const float uwarp = kdtita*(tita_w + 0.5*fovh);

			//The warped pixel (which is not integer in general) contributes to all the surrounding ones
            if ((uwarp >= 0.f)&&(uwarp < cols_lim))
			{
				const int uwarp_l = uwarp;
				const int uwarp_r = uwarp_l + 1;
				const float delta_r = float(uwarp_r) - uwarp;
				const float delta_l = uwarp - float(uwarp_l);

				//Very close pixel
				if (abs(round(uwarp) - uwarp) < 0.05f)
				{
					range_warped[image_level](round(uwarp)) += range_w;
					wacu(round(uwarp)) += 1.f;
				}
				else
				{
					const float w_r = square(delta_l);
					range_warped[image_level](uwarp_r) += w_r*range_w;
					wacu(uwarp_r) += w_r;

					const float w_l = square(delta_r);
					range_warped[image_level](uwarp_l) += w_l*range_w;
					wacu(uwarp_l) += w_l;
				}
			}
		}
	}

	//Scale the averaged range and compute coordinates
	for (unsigned int u = 0; u<cols_i; u++)
	{	
		if (wacu(u) > 0.f)
		{
			const float tita = -0.5f*fovh + float(u)/kdtita;
			range_warped[image_level](u) /= wacu(u);
			xx_warped[image_level](u) = range_warped[image_level](u)*cos(tita);
			yy_warped[image_level](u) = range_warped[image_level](u)*sin(tita);
		}
		else
		{
			range_warped[image_level](u) = 0.f;
			xx_warped[image_level](u) = 0.f;
			yy_warped[image_level](u) = 0.f;
		}
	}
}

void RF2O::odometryCalculation()
{
	//==================================================================================
	//						DIFERENTIAL  ODOMETRY  MULTILEVEL
	//==================================================================================

    clock.Tic();
    createScanPyramid();

    //Coarse-to-fine scheme
    for (unsigned int i=0; i<ctf_levels; i++)
    {
        //Previous computations
        transformations[i].setIdentity();

        level = i;
        unsigned int s = pow(2.f,int(ctf_levels-(i+1)));
        cols_i = ceil(float(cols)/float(s));
        image_level = ctf_levels - i + round(log2(round(float(width)/float(cols)))) - 1;

        //1. Perform warping
        if (i == 0)
        {
            range_warped[image_level] = range[image_level];
            xx_warped[image_level] = xx[image_level];
            yy_warped[image_level] = yy[image_level];
        }
        else
            performWarping();


        //2. Calculate inter coords
        calculateCoord();

        //3. Compute derivatives
        calculaterangeDerivativesSurface();

        //4. Compute weights
        computeWeights();

        //5. Solve odometry
        if (num_valid_range > 3)
        {
            if (test)
                //solveSystemMCauchy();
                solveSystemSmoothTruncQuad();
                //solveSystemQuadResiduals();
                //solveSystemSmoothTruncQuadNoPreW2();
                //solveSystemQuadResidualsNoPreW();
            else
                //solveSystemMTukey();
                //solveSystemMCauchy();
                //solveSystemTruncatedQuad();
                //solveSystemQuadResiduals();
                solveSystemSmoothTruncQuad();
                //solveSystemSmoothTruncQuadFromBeginning();
                //solveSystemQuadResiduals();
        }

        //6. Filter solution
        filterLevelSolution();
    }

    runtime = 1000.f*clock.Tac();
    cout << endl << "Time odometry (ms): " << runtime;

    //Update poses
    PoseUpdate();
}

void RF2O::filterLevelSolution()
{
	//		Calculate Eigenvalues and Eigenvectors
	//----------------------------------------------------------
    SelfAdjointEigenSolver<Matrix3f> eigensolver(cov_odo);
	if (eigensolver.info() != Success) 
	{ 
        printf("\n Eigensolver couldn't find a solution. Pose is not updated");
		return;
	}
	
	//First, we have to describe both the new linear and angular speeds in the "eigenvector" basis
	//-------------------------------------------------------------------------------------------------
    Matrix3f Bii = eigensolver.eigenvectors();
    Vector3f kai_b = Bii.colPivHouseholderQr().solve(kai_loc_level);


	//Second, we have to describe both the old linear and angular speeds in the "eigenvector" basis too
	//-------------------------------------------------------------------------------------------------
    Vector3f kai_loc_sub;

	//Important: we have to substract the solutions from previous levels
    Matrix3f acu_trans = Matrix3f::Identity();
	for (unsigned int i=0; i<level; i++)
		acu_trans = transformations[i]*acu_trans;

    kai_loc_sub(0) = -acu_trans(0,2);
    kai_loc_sub(1) = -acu_trans(1,2);
	if (acu_trans(0,0) > 1.f)
		kai_loc_sub(2) = 0.f;
	else
        kai_loc_sub(2) = -acos(acu_trans(0,0))*sign(acu_trans(1,0));
	kai_loc_sub += kai_loc_old;

    Vector3f kai_b_old = Bii.colPivHouseholderQr().solve(kai_loc_sub);

	//Filter speed
    const float cf = 15e3f*expf(-int(level)), df = 0.05f*expf(-int(level));
    //const float cf = 0.f, df = 0.f;

    Vector3f kai_b_fil;
	for (unsigned int i=0; i<3; i++)
	{
        kai_b_fil(i,0) = (kai_b(i,0) + (cf*eigensolver.eigenvalues()(i,0) + df)*kai_b_old(i,0))/(1.f + cf*eigensolver.eigenvalues()(i,0) + df);
        //kai_b_fil_f(i,0) = (1.f*kai_b(i,0) + 0.f*kai_b_old_f(i,0))/(1.0f + 0.f);
	}

	//Transform filtered speed to local reference frame and compute transformation
    Vector3f kai_loc_fil = Bii.inverse().colPivHouseholderQr().solve(kai_b_fil);

	//transformation
    const float incrx = kai_loc_fil(0);
    const float incry = kai_loc_fil(1);
    const float rot = kai_loc_fil(2);
	transformations[level](0,0) = cos(rot);
	transformations[level](0,1) = -sin(rot);
	transformations[level](1,0) = sin(rot);
	transformations[level](1,1) = cos(rot);

    //With my linearization I should not use Lie algebra
    Matrix2f V = Matrix2f::Identity();
    Vector2f incr; incr << incrx, incry;
    if (abs(rot) > 0.001f)
    {
        const float V1 = sin(rot)/rot;
        const float V2 = (1.f - cos(rot))/rot;
        V << V1, -V2, V2, V1;
    }

    transformations[level](0,2) = (V*incr)(0);
    transformations[level](1,2) = (V*incr)(1);
}

void RF2O::PoseUpdate()
{
	//First, compute the overall transformation
	//---------------------------------------------------
    Matrix3f acu_trans = Matrix3f::Identity();
	for (unsigned int i=1; i<=ctf_levels; i++)
		acu_trans = transformations[i-1]*acu_trans;


	//				Compute kai_loc and kai_abs
	//--------------------------------------------------------
    kai_loc(0) = acu_trans(0,2);
    kai_loc(1) = acu_trans(1,2);
	if (acu_trans(0,0) > 1.f)
		kai_loc(2) = 0.f;
	else
        kai_loc(2) = acos(acu_trans(0,0))*sign(acu_trans(1,0));

    float phi = laser_pose.phi();

	kai_abs(0) = kai_loc(0)*cos(phi) - kai_loc(1)*sin(phi);
	kai_abs(1) = kai_loc(0)*sin(phi) + kai_loc(1)*cos(phi);
	kai_abs(2) = kai_loc(2);


	//						Update poses
	//-------------------------------------------------------
	laser_oldpose = laser_pose;
    mrpt::poses::CPose2D pose_aux_2D(acu_trans(0,2), acu_trans(1,2), kai_loc(2));
	laser_pose = laser_pose + pose_aux_2D;



    //                  Compute kai_loc_old
	//-------------------------------------------------------
	phi = laser_pose.phi();
	kai_loc_old(0) = kai_abs(0)*cos(phi) + kai_abs(1)*sin(phi);
	kai_loc_old(1) = -kai_abs(0)*sin(phi) + kai_abs(1)*cos(phi);
	kai_loc_old(2) = kai_abs(2);
}
