// [[Rcpp::depends(dqrng, BH, sitmo)]]
// [[Rcpp::plugins(cpp11)]]

#include <mystdint.h>
#include <dqrng_generator.h>
#include <dqrng_distribution.h>
#include <pcg_random.hpp>
#include <convert_seed.h>
#include <R_randgen.h>
#include <minimal_int_set.h>
#include <dqrng.h>
#include <Rcpp.h>
#ifdef __linux__
#include <execinfo.h> // backtrace, backtrace_symbols
#endif
#include "NBI_distribution.h"
#include "aux_functions.h"

using namespace Rcpp;
using namespace std;


// === preprocessor macro for Rcpp Vectors on out-of-boundary element access (select one from below) ===

// below gives ERROR with CALL STACK - significantly benefitting error investigation. Safe: as program stops on error.
#define VECT_ELEM(__thisVector__,__thisElem__) VectElem(__thisVector__,__thisElem__)

// below gives SIMPLE ERROR (no call stack). Safe: as program stops on error.
// #define VECT_ELEM(__thisVector__,__thisElem__) __thisVector__(__thisElem__)

// below gives NO ERROR. Unsafe: program may continue incorrectly (using random data, with no error revealed) or crash.
// #define VECT_ELEM(__thisVector__,__thisElem__) __thisVector__[__thisElem__]

// === END preprocessor macro ===

// combine 2 uint to a uint64
long long u32tou64(const unsigned int low, const unsigned int high)
{
  return (((uint64_t) high) << 32) | ((uint64_t) low);
}


// Prepare the rng
namespace
{
  dqrng::rng64_t init()
  {
    Rcpp::RNGScope rngScope;
    Rcpp::IntegerVector seed(2, dqrng::R_random_int);
    return dqrng::generator(dqrng::convert_seed<uint64_t>(seed));
  }
  dqrng::rng64_t rng = init();

  using generator = double(*)();
  dqrng::uniform_distribution uniform{};
  generator runif_impl = [] () {return uniform(*rng);};
}

// for disease influence
struct infl
{
  vector<IntegerVector> disease_prvl;
  vector<NumericVector> mltp;
  vector<int> lag;
};

// for parameters from GAMLSS models
struct distr_prm_vr
{
  double intercept;
  double log_age_coef;
  double log_year_coef;
  NumericVector sex_coef;
  NumericVector dimd_coef;
};

// for parameters from GAMLSS models
struct duration_prm
{
  distr_prm_vr mu;
  distr_prm_vr sigma;
  distr_prm_vr nu;
};

struct disease_epi
{
  string type;
  IntegerVector prvl;
  NumericVector prbl1; // for 1st year case fatality
  NumericVector prbl2; // for prevalent cases case fatality
  infl influenced_by;
  duration_prm dur_forward;
  CharacterVector aggregate;
  double mm_wt;
  bool can_recur;
  bool flag; // set true the first time incidence occurs & set true to cure in the next year
  int cure;
  int death_code;
};

struct disease_meta
{
  disease_epi incd;
  disease_epi dgns;
  disease_epi mrtl;
  bool mrtl1flag;
  int seed;
};

struct simul_meta
{
  int init_year;
  int age_low;
  IntegerVector pid;
  IntegerVector year;
  IntegerVector age;
  IntegerVector sex; // 1 = men, 2 = women
  IntegerVector dimd; // 1 = 1 most deprived
  IntegerVector dead;
  IntegerVector mm_count;
  NumericVector mm_score;
};

simul_meta get_simul_meta(const List l, DataFrame dt)
{
  simul_meta out = {};
  out.init_year  = as<int>(l["init_year"]);
  out.age_low  = as<int>(l["ageL"]);
  out.pid = dt[as<string>(l["pids"])];
  out.year = dt[as<string>(l["years"])];
  out.age = dt[as<string>(l["ages"])];
  out.sex = dt[as<string>(l["sexs"])];
  out.dimd = dt[as<string>(l["dimds"])];
  out.dead = dt[as<string>(l["all_cause_mrtl"])];
  out.mm_count = dt[as<string>(l["cms_count"])];
  out.mm_score = dt[as<string>(l["cms_score"])];
  return out;
}

/*template<class T> T& RcppVectorElem(Vector &v,int index) {
	try { return v(index); }
	catch(index_out_of_bounds e) { 
		e.message+= "mbirkett";
		throw e; 
	}
}*/

#ifdef __linux__
string GetStackTrace(void) {
	const int maxNumStackCalls= 1024;
	void *stackAddresses[maxNumStackCalls];
	int numStackCalls= backtrace(stackAddresses,maxNumStackCalls);
	char **stackCallDescriptions= backtrace_symbols(stackAddresses,numStackCalls);
	if(stackCallDescriptions==NULL)
		return string("[No stack trace available].");
	else {
		string stackTrace;
		for(int thisStackCall=0;thisStackCall<numStackCalls;++thisStackCall) {
			stackTrace= (stackTrace+stackCallDescriptions[thisStackCall])+"\n";
		}
		free(stackCallDescriptions);
		return stackTrace;
	}
}

#elif _WIN32
string GetStackTrace(void) {
	return string("[No stack trace available on Windows].");
}
#endif

IntegerVector::Proxy VectElem(IntegerVector &v,int index) {
	try { return v(index); }
	catch(index_out_of_bounds e) { 
		throw out_of_range( string(e.what())+"\n"+GetStackTrace() );
	}
}

NumericVector::Proxy VectElem(NumericVector &v,int index) {
	try { return v(index); }
	catch(index_out_of_bounds e) { 
		throw out_of_range( string(e.what())+"\n"+GetStackTrace() );
	}
}

CharacterVector::Proxy VectElem(CharacterVector &v,int index) {
	try { return v(index); }
	catch(index_out_of_bounds e) { 
		throw out_of_range( string(e.what())+"\n"+GetStackTrace() );
	}
}

/** Prepare a specific Disease's disease_meta object largely via Disease.to_cpp().
 @param diseaseFields Disease fields generated by Disease.to_cpp(), e.g. incidence, diagnosis, mortality, seed, diseaseName.
 @param dtSynthPop The SynthPop$pop population table.
 @return A disease_meta object filled for this disease.
*/
disease_meta get_disease_meta(const List diseaseFields, DataFrame dtSynthPop)
{
	// copy disease name necessary, else Rcpp seems to give reference to internal buffer which later will have other values
	string diseaseName;
	bool haveDiseaseName= diseaseFields.containsElementNamed("diseaseName");
	if(haveDiseaseName)diseaseName= as<string>(diseaseFields["diseaseName"]);

  disease_meta out = {};

  List incd, dgns, mrtl;


  // incidence
  if (diseaseFields.containsElementNamed("incidence"))
  {
    incd = diseaseFields["incidence"];
    out.incd.type =  as<string>(incd["type"]);

    if (incd.containsElementNamed("prevalence")) out.incd.prvl = dtSynthPop[as<string>(incd["prevalence"])];
    if (incd.containsElementNamed("probability"))out.incd.prbl1 = dtSynthPop[as<string>(incd["probability"])];

    if (incd.containsElementNamed("aggregate")) out.incd.aggregate = as<CharacterVector>(incd["aggregate"]);

    if (incd.containsElementNamed("influenced_by"))
    {
      List ib = incd["influenced_by"];
      CharacterVector tmps= ib.names();
      int n = ib.length();
      List ibb;
      if (out.incd.type == "Type0")
      {
        for (int i = 0; i < n; ++i)
        {
          ibb = ib[i];
          out.incd.influenced_by.disease_prvl.push_back(dtSynthPop[as<string>(tmps[i])]);
          out.incd.influenced_by.lag.push_back(as<int>(ibb["lag"])); // set to 0 from R side
        }
      }
      else
      {
        for (int i = 0; i < n; ++i)
        {
          ibb = ib[i];
          out.incd.influenced_by.disease_prvl.push_back(dtSynthPop[as<string>(tmps[i])]);
          out.incd.influenced_by.mltp.push_back(dtSynthPop[as<string>(ibb["multiplier"])]);
          out.incd.influenced_by.lag.push_back(as<int>(ibb["lag"]));
        }
      }


    }
    out.incd.flag = false;
    if (incd.containsElementNamed("can_recur")) out.incd.can_recur = as<bool>(incd["can_recur"]);
    else out.incd.can_recur = false;
  }

  // diagnosis
  if (diseaseFields.containsElementNamed("diagnosis"))
  {
    dgns = diseaseFields["diagnosis"];


    out.dgns.type  = as<string>(dgns["type"]);
	if(!dgns.containsElementNamed("mm_wt") || dgns["mm_wt"]==R_NilValue) // latter test: is [mm_wt]==NULL?
	{
		string errorMessage= "Missing [meta].[diagnosis].[mm_wt] property for "+
			(haveDiseaseName?diseaseName:(string)"unknown")+
			". Add [mm_wt] property for this disease in .yaml config file.\n";
		throw std::runtime_error(errorMessage);
	}
    out.dgns.mm_wt = as<double>(dgns["mm_wt"]);
    if (dgns.containsElementNamed("diagnosed")) out.dgns.prvl  = dtSynthPop[as<string>(dgns["diagnosed"])];
    if (dgns.containsElementNamed("probability")) out.dgns.prbl1 = dtSynthPop[as<string>(dgns["probability"])];

    if (dgns.containsElementNamed("duration_distr_forwards")) // an indirect feature of recurrence
    {
      List ib = dgns["duration_distr_forwards"];
      List pr = ib["mu"];
      out.dgns.dur_forward.mu.intercept = as<double>(pr["intercept"]);
      out.dgns.dur_forward.mu.log_age_coef = as<double>(pr["log(age)"]);
      out.dgns.dur_forward.mu.log_year_coef = as<double>(pr["log(year)"]);
      out.dgns.dur_forward.mu.sex_coef = as<NumericVector>(pr["sex"]);
      out.dgns.dur_forward.mu.dimd_coef = as<NumericVector>(pr["dimd"]);

      pr = ib["sigma"];
      out.dgns.dur_forward.sigma.intercept = as<double>(pr["intercept"]);
      out.dgns.dur_forward.sigma.log_age_coef = as<double>(pr["log(age)"]);
      out.dgns.dur_forward.sigma.log_year_coef = as<double>(pr["log(year)"]);
      out.dgns.dur_forward.sigma.sex_coef = as<NumericVector>(pr["sex"]);
      out.dgns.dur_forward.sigma.dimd_coef = as<NumericVector>(pr["dimd"]);

      pr = ib["nu"];
      out.dgns.dur_forward.nu.intercept = as<double>(pr["intercept"]);
      out.dgns.dur_forward.nu.log_age_coef = as<double>(pr["log(age)"]);
      out.dgns.dur_forward.nu.log_year_coef = as<double>(pr["log(year)"]);
      out.dgns.dur_forward.nu.sex_coef = as<NumericVector>(pr["sex"]);
      out.dgns.dur_forward.nu.dimd_coef = as<NumericVector>(pr["dimd"]);

      out.dgns.flag = true; // denotes asthma-like disease
    }
    else
    { // if duration forward doesn't exist
      out.dgns.dur_forward.mu.intercept = 0.0;
      out.dgns.dur_forward.mu.log_age_coef = 0.0;
      out.dgns.dur_forward.mu.log_year_coef = 0.0;
      out.dgns.dur_forward.mu.sex_coef = {0.0, 0.0};
      out.dgns.dur_forward.mu.dimd_coef = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

      out.dgns.dur_forward.sigma.intercept = 0.0;
      out.dgns.dur_forward.sigma.log_age_coef = 0.0;
      out.dgns.dur_forward.sigma.log_year_coef = 0.0;
      out.dgns.dur_forward.sigma.sex_coef ={0.0, 0.0};
      out.dgns.dur_forward.sigma.dimd_coef = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

      out.dgns.dur_forward.nu.intercept = 0.0;
      out.dgns.dur_forward.nu.log_age_coef = 0.0;
      out.dgns.dur_forward.nu.log_year_coef = 0.0;
      out.dgns.dur_forward.nu.sex_coef = {0.0, 0.0};
      out.dgns.dur_forward.nu.dimd_coef = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

      out.dgns.flag = false;
    }


    if (out.dgns.type == "Type0")
    {
      List ib = dgns["influenced_by"];
      CharacterVector tmps= ib.names();
      int n = ib.length();
      List ibb;
      for (int i = 0; i < n; ++i)
      {
        ibb = ib[i];
        out.dgns.influenced_by.disease_prvl.push_back(dtSynthPop[as<string>(tmps[i])]);
      }
    }

    out.dgns.cure = 0; // to hold the stochastic disease duration
  }

  // mortality
  if (diseaseFields.containsElementNamed("mortality"))
  {
    mrtl = diseaseFields["mortality"];
    out.mrtl.type = as<string>(mrtl["type"]);

    if (mrtl.containsElementNamed("probability"))
    {
      out.mrtl.prbl2 = dtSynthPop[as<string>(mrtl["probability"])];
      out.mrtl1flag = false;
    }
    if (mrtl.containsElementNamed("probability1styear"))
    {
      out.mrtl.prbl1 =  dtSynthPop[as<string>(mrtl["probability1styear"])];
      out.mrtl1flag = true;
    }
    if (mrtl.containsElementNamed("cure")) out.mrtl.cure = as<int>(mrtl["cure"]);
    else out.mrtl.cure = -1; // NOTE 0 has special meaning for i.e. asthma
    if (mrtl.containsElementNamed("code")) out.mrtl.death_code = as<int>(mrtl["code"]);

    if (mrtl.containsElementNamed("influenced_by"))
    {
      List ib = mrtl["influenced_by"];
      CharacterVector tmps= ib.names();
      int n = ib.length();
      List ibb;
      for (int i = 0; i < n; ++i)
      {
        ibb = ib[i];
        out.mrtl.influenced_by.disease_prvl.push_back(dtSynthPop[as<string>(tmps[i])]);
        out.mrtl.influenced_by.mltp.push_back(dtSynthPop[as<string>(ibb["multiplier"])]);
        out.mrtl.influenced_by.lag.push_back(as<int>(ibb["lag"]));
      }
    }

    out.mrtl.flag = false;

  }

  out.seed = as<int>(diseaseFields["seed"]);
  return out;
}

int get_dur_forward (const int i, // represents the row
                     const double& rn, // random number
                     const disease_meta& ds,
                     const simul_meta& sm)
{
  double mu = exp(ds.dgns.dur_forward.mu.intercept +
                  ds.dgns.dur_forward.mu.log_age_coef * log(sm.age[i]) +
                  ds.dgns.dur_forward.mu.log_year_coef * log(sm.year[i]) +
                  ds.dgns.dur_forward.mu.sex_coef[sm.sex[i] - 1] +
                  ds.dgns.dur_forward.mu.dimd_coef[sm.dimd[i] - 1]);
  double sigma = exp(ds.dgns.dur_forward.sigma.intercept +
                  ds.dgns.dur_forward.sigma.log_age_coef * log(sm.age[i]) +
                  ds.dgns.dur_forward.sigma.log_year_coef * log(sm.year[i]) +
                  ds.dgns.dur_forward.sigma.sex_coef[sm.sex[i] - 1] +
                  ds.dgns.dur_forward.sigma.dimd_coef[sm.dimd[i] - 1]);
  double nu = antilogit(ds.dgns.dur_forward.nu.intercept +
                  ds.dgns.dur_forward.nu.log_age_coef * log(sm.age[i]) +
                  ds.dgns.dur_forward.nu.log_year_coef * log(sm.year[i]) +
                  ds.dgns.dur_forward.nu.sex_coef[sm.sex[i] - 1] +
                  ds.dgns.dur_forward.nu.dimd_coef[sm.dimd[i] - 1]
  );

  return 1 + my_qZANBI_scalar(rn, mu, sigma, nu, true, false, true);
}

int get_dur_forward_prvl (const int& i, // represents the row
                     const double& rn, // random number
                     const int& prvl_dur, // used only for prevalent cases that enter the simulation
                     const disease_meta& ds,
                     const simul_meta& sm)
{
  double mu = exp(ds.dgns.dur_forward.mu.intercept +
                  ds.dgns.dur_forward.mu.log_age_coef * log(sm.age[i]) +
                  ds.dgns.dur_forward.mu.log_year_coef * log(sm.year[i]) +
                  ds.dgns.dur_forward.mu.sex_coef[sm.sex[i] - 1] +
                  ds.dgns.dur_forward.mu.dimd_coef[sm.dimd[i] - 1]);
  double sigma = exp(ds.dgns.dur_forward.sigma.intercept +
                     ds.dgns.dur_forward.sigma.log_age_coef * log(sm.age[i]) +
                     ds.dgns.dur_forward.sigma.log_year_coef * log(sm.year[i]) +
                     ds.dgns.dur_forward.sigma.sex_coef[sm.sex[i] - 1] +
                     ds.dgns.dur_forward.sigma.dimd_coef[sm.dimd[i] - 1]);
  double nu = antilogit(ds.dgns.dur_forward.nu.intercept +
                        ds.dgns.dur_forward.nu.log_age_coef * log(sm.age[i]) +
                        ds.dgns.dur_forward.nu.log_year_coef * log(sm.year[i]) +
                        ds.dgns.dur_forward.nu.sex_coef[sm.sex[i] - 1] +
                        ds.dgns.dur_forward.nu.dimd_coef[sm.dimd[i] - 1]
  );

  double thresh = my_pZANBI_scalar(prvl_dur,mu, sigma, nu, true, false, true);
  if (rn < thresh)
  {
    return prvl_dur + my_qZANBI_scalar(1 - rn, mu, sigma, nu, true, false, true);  }
  else
  {
    return prvl_dur + my_qZANBI_scalar(rn, mu, sigma, nu, true, false, true);
  }
}

inline void DiseaseIncidenceType2(vector<disease_meta> &dsmeta,int j, int i, double rn1)
{
	 if (dsmeta[j].incd.can_recur) // no need to check incd.flag
	 {
		if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) == 0 && rn1 <= VECT_ELEM(dsmeta[j].incd.prbl1,i))
		{
			VECT_ELEM(dsmeta[j].incd.prvl,i) = 1;
		}

		if (dsmeta[j].mrtl.type == "Type2" || dsmeta[j].mrtl.type == "Type4")
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0 &&
				VECT_ELEM(dsmeta[j].incd.prvl,i - 1) < dsmeta[j].mrtl.cure)
					VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		}
		else
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0)
				VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		  // Above holds irrespective of if disease can be cured as
		  // VECT_ELEM(dsmeta[j].incd.prvl,i - 1) cannot go above duration for cure
		}

	 }
	 else // no recurrence
	 {
		if (!dsmeta[j].incd.flag && VECT_ELEM(dsmeta[j].incd.prvl,i - 1) == 0 &&
			 rn1 <= VECT_ELEM(dsmeta[j].incd.prbl1,i))
		{
			VECT_ELEM(dsmeta[j].incd.prvl,i) = 1;
		  dsmeta[j].incd.flag = true; // Set flag
		}

		if (dsmeta[j].mrtl.type == "Type2" || dsmeta[j].mrtl.type == "Type4")
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0 &&
				VECT_ELEM(dsmeta[j].incd.prvl,i - 1) < dsmeta[j].mrtl.cure)
					VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		}
		else
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0)
				VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		  // Above holds irrespective of if disease can be cured as
		  // VECT_ELEM(dsmeta[j].incd.prvl,i - 1) cannot go above duration for cure
		}
	 }
}

inline void DiseaseIncidenceType3(vector<disease_meta> &dsmeta,int i, int j, double& mltp, double rn1, simul_meta &meta)
{
	 for (vector<IntegerVector>::size_type k = 0; k < dsmeta[j].incd.influenced_by.disease_prvl.size(); ++k) // Loop over influenced by diseases
	 {
		// if lag > 0 look back. For diseases depending on self, lag = 0 and
		// that triggers the use of the .incd.flag
		if ((dsmeta[j].incd.influenced_by.lag[k] > 0 && VECT_ELEM(dsmeta[j].incd.influenced_by.disease_prvl[k],i - dsmeta[j].incd.influenced_by.lag[k]) > 0) || 
			(dsmeta[j].incd.influenced_by.lag[k] == 0 && dsmeta[j].incd.flag))
		{
		  mltp *= VECT_ELEM(dsmeta[j].incd.influenced_by.mltp[k],i); // no lag here
		}
	 }

	 if (dsmeta[j].incd.can_recur) // no need to check incd.flag
	 {
		if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) == 0 && rn1 <= VECT_ELEM(dsmeta[j].incd.prbl1,i) * mltp)
		{ // if new disease case
			VECT_ELEM(dsmeta[j].incd.prvl,i) = 1;
		  // dsmeta[j].dgns.cure holds the duration_prm for this spell
		  // NOTE dsmeta[j].incd.influenced_by.lag[k] == 0 identifies diseases like asthma
		  if (dsmeta[j].dgns.flag)  dsmeta[j].dgns.cure = get_dur_forward(i, runif_impl(), dsmeta[j], meta);
		}

		// Below is the logic for diseases like asthma to cure prevalent
		// cases in initial year. NOTE I don't add 1 to duration
		// intentionally, to allow 0s.
		if (dsmeta[j].dgns.flag &&
			 (meta.year[i] == meta.init_year || meta.age[i] == meta.age_low) &&
			 VECT_ELEM(dsmeta[j].incd.prvl,i) > 1 ) // >1 to exclude incident cases from above.
		{
		  dsmeta[j].dgns.cure = get_dur_forward_prvl(i, runif_impl(), VECT_ELEM(dsmeta[j].incd.prvl,i), dsmeta[j], meta);
		}

		// Logic to advance duration of prevalent cases by 1
		if ((dsmeta[j].mrtl.type == "Type2" || dsmeta[j].mrtl.type == "Type4") &&
			 dsmeta[j].mrtl.cure > 0) // deterministic duration, i.e. for cancers
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0 &&
				VECT_ELEM(dsmeta[j].incd.prvl,i - 1) < dsmeta[j].mrtl.cure)
					VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		}
		else if ((dsmeta[j].mrtl.type == "Type2" || dsmeta[j].mrtl.type == "Type4") &&
			 dsmeta[j].mrtl.cure == 0 && dsmeta[j].dgns.flag) // stochastic duration, i.e. for asthma
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0 &&
				VECT_ELEM(dsmeta[j].incd.prvl,i - 1) < dsmeta[j].dgns.cure)
			 VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		}
		else
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0)
					VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		  // Above holds irrespective of if disease can be cured as
		  // VECT_ELEM(dsmeta[j].incd.prvl,i - 1) cannot go above duration for cure
		}

	 }
	 else // no recurrence
	 {
		if (!dsmeta[j].incd.flag && VECT_ELEM(dsmeta[j].incd.prvl,i - 1) == 0 &&
			 rn1 <= VECT_ELEM(dsmeta[j].incd.prbl1,i) * mltp)
		{
			VECT_ELEM(dsmeta[j].incd.prvl,i) = 1;
		  dsmeta[j].incd.flag = true; // Set flag
		}

		if (dsmeta[j].mrtl.type == "Type2" || dsmeta[j].mrtl.type == "Type4")
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0 &&
				VECT_ELEM(dsmeta[j].incd.prvl,i - 1) < dsmeta[j].mrtl.cure)
			 VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		}
		else
		{
		  if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0)
			 VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		  // Above holds irrespective of if disease can be cured as
		  // VECT_ELEM(dsmeta[j].incd.prvl,i - 1) cannot go above duration for cure
		}
	 }

	 mltp = 1.0;
}

inline void EvalDiseaseIncidence(vector<disease_meta> &dsmeta,int i, int j, double rn1, double& mltp, simul_meta &meta)
{
	try {
	  if (dsmeta[j].incd.type == "Type0")
	  {
		 for (vector<IntegerVector>::size_type k = 0; k < dsmeta[j].incd.influenced_by.disease_prvl.size(); ++k) // Loop over influenced by diseases
		 {
			if (VECT_ELEM(dsmeta[j].incd.influenced_by.disease_prvl[k],i) > VECT_ELEM(dsmeta[j].incd.prvl,i))
			{
				VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.influenced_by.disease_prvl[k],i);
			}
		 }
	  }

	  if (dsmeta[j].incd.type == "Type1")
	  { // NOTE Type 1 doesn't need to use flags for recurrence
		 if (dsmeta[j].incd.can_recur)
		 {
			if (VECT_ELEM(dsmeta[j].incd.prbl1,i) == 1.0) // logic overwrites prvl for init year
			VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		 }
		 else // if no recurrence assuming no cure
		 {
			if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) == 0 && VECT_ELEM(dsmeta[j].incd.prbl1,i) == 1.0)
				VECT_ELEM(dsmeta[j].incd.prvl,i) = 1;
			if (VECT_ELEM(dsmeta[j].incd.prvl,i - 1) > 0)
				VECT_ELEM(dsmeta[j].incd.prvl,i) = VECT_ELEM(dsmeta[j].incd.prvl,i - 1) + 1;
		 }
	  }

	  else if (dsmeta[j].incd.type == "Type2")
			DiseaseIncidenceType2(dsmeta,j, i, rn1);

	  else if (dsmeta[j].incd.type == "Type3") // NOTE I don't need this type. Can be replaced by a flag to notify disease dependence
			DiseaseIncidenceType3(dsmeta,i, j, mltp, rn1, meta);

	  else if (dsmeta[j].incd.type == "Type4")
	  {
		 // TODO
	  }

	  else if (dsmeta[j].incd.type == "Type5")
	  {
		 // TODO
	  }
  }
	catch(std::exception &e) { 
		// forward standard library exceptions as runtime_errors - all these have a explanatory message which Rcpp prints.
		throw std::runtime_error((string)"Error within EvalDiseaseIncidence(): "+e.what());
	}
}

inline void EvalDiagnosis(vector<disease_meta> &dsmeta,double& rn1, int j, int i, simul_meta& meta)
{
	try {
	  rn1 = runif_impl();

	  if (dsmeta[j].incd.type != "Universal" &&VECT_ELEM(dsmeta[j].incd.prvl,i) > 0 && dsmeta[j].dgns.type == "Type0")
	  {
		 for (vector<IntegerVector>::size_type k = 0; k < dsmeta[j].dgns.influenced_by.disease_prvl.size(); ++k) // Loop over influenced by diseases
		 {
			if (dsmeta[j].dgns.influenced_by.disease_prvl[k](i) > VECT_ELEM(dsmeta[j].dgns.prvl,i))
			{
				VECT_ELEM(dsmeta[j].dgns.prvl,i) = dsmeta[j].dgns.influenced_by.disease_prvl[k](i);
			}
		 }
	  }

	  else if (dsmeta[j].incd.type != "Universal" && VECT_ELEM(dsmeta[j].incd.prvl,i) > 0 && dsmeta[j].dgns.type == "Type1") // enter branch only for prevalent cases
	  {
		 if (VECT_ELEM(dsmeta[j].dgns.prvl,i - 1) == 0 && rn1 <= VECT_ELEM(dsmeta[j].dgns.prbl1,i))
		 {
			VECT_ELEM(dsmeta[j].dgns.prvl,i) = 1;
		 }
		 if (VECT_ELEM(dsmeta[j].dgns.prvl,i - 1) > 0)
		 {
				VECT_ELEM(dsmeta[j].dgns.prvl,i) = VECT_ELEM(dsmeta[j].dgns.prvl,i - 1) + 1;
		 }
	  }

	  if ((dsmeta[j].dgns.type == "Type0" || dsmeta[j].dgns.type == "Type1") && VECT_ELEM(dsmeta[j].dgns.prvl,i) > 0 && dsmeta[j].dgns.mm_wt > 0.0) meta.mm_score[i] += dsmeta[j].dgns.mm_wt;
	  if ((dsmeta[j].dgns.type == "Type0" || dsmeta[j].dgns.type == "Type1") && VECT_ELEM(dsmeta[j].dgns.prvl,i) > 0 && dsmeta[j].dgns.mm_wt > 0.0) meta.mm_count[i]++;
  }
	catch(std::exception &e) { 
		// forward standard library exceptions as runtime_errors - all these have a explanatory message which Rcpp prints.
		throw std::runtime_error((string)"Error within EvalDiagnosis(): "+e.what());
	}
}

inline void EvalMortality(vector<disease_meta> &dsmeta,vector<int> &tempdead,int i, int j, double& rn1, double& mltp)
{
	try {
	  rn1 = runif_impl();

	  if (dsmeta[j].incd.type == "Universal" || VECT_ELEM(dsmeta[j].incd.prvl,i) > 0) // enter branch only for prevalent cases or Universal incidence
	  {
		 // Type 1 mortality (no cure, no disease dependency)
		 if (dsmeta[j].mrtl.type == "Type1")
		 {
			if (dsmeta[j].mrtl1flag)  // Type universal never enters this branch
			{
			  if (VECT_ELEM(dsmeta[j].incd.prvl,i) == 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl1,i)) tempdead.push_back(dsmeta[j].mrtl.death_code);
			  if (VECT_ELEM(dsmeta[j].incd.prvl,i) > 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i)) tempdead.push_back(dsmeta[j].mrtl.death_code);
			}
			else if (rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i)) tempdead.push_back(dsmeta[j].mrtl.death_code); // only prevalent cases enter this branch
		 }// End Type 1


		 // Type 2 mortality (cure, no disease dependency). Not compatible with
		 // universal incidence (would be meaningless as Type 2 mortality
		 // allows cure). NOTE cure with disease dependence is type 4
		 else if (dsmeta[j].mrtl.type == "Type2")
		 {
			if (VECT_ELEM(dsmeta[j].incd.prvl,i) <= (dsmeta[j].dgns.flag ? dsmeta[j].dgns.cure : dsmeta[j].mrtl.cure)) // Valid since not universal incidence
			{
			  if (dsmeta[j].mrtl1flag)  // if fatality for incident cases estimated separately
			  {
				 if (VECT_ELEM(dsmeta[j].incd.prvl,i) == 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl1,i)) tempdead.push_back(dsmeta[j].mrtl.death_code);
				 if (VECT_ELEM(dsmeta[j].incd.prvl,i) > 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i)) tempdead.push_back(dsmeta[j].mrtl.death_code);
			  }
			  else if (rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i)) tempdead.push_back(dsmeta[j].mrtl.death_code); // only prevalent cases enter this branch
			}
			else dsmeta[j].mrtl.flag = true; // if alive cure after defined period

		 } // End Type 2 mortality

		 // Type 3 mortality (no cure, disease dependency)
		 else if (dsmeta[j].mrtl.type == "Type3")
		 {
			for (vector<IntegerVector>::size_type k = 0; k < dsmeta[j].mrtl.influenced_by.disease_prvl.size(); ++k) // Loop over influenced by diseases
			{
			  if (VECT_ELEM(dsmeta[j].mrtl.influenced_by.disease_prvl[k],i - dsmeta[j].mrtl.influenced_by.lag[k]) > 0)
			  {
				 mltp *= dsmeta[j].mrtl.influenced_by.mltp[k](i); // no lag here
			  }
			}

			if (dsmeta[j].mrtl1flag)  // if fatality for incident cases estimated separately
			{
			  if (VECT_ELEM(dsmeta[j].incd.prvl,i) == 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl1,i))        tempdead.push_back(dsmeta[j].mrtl.death_code); // mltp not needed here
			  if (VECT_ELEM(dsmeta[j].incd.prvl,i)  > 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i) * mltp) tempdead.push_back(dsmeta[j].mrtl.death_code);
			}
			else // if (dsmeta[j].incd.type == "Universal" || !dsmeta[j].mrtl1flag). NOTE Universal enters this branch as well.
			{
			  if (rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i) * mltp) tempdead.push_back(dsmeta[j].mrtl.death_code);
			}

			mltp = 1.0;
		 } // End Type 3 mortality

		 // Type 4 mortality (cure, disease dependency). Note that universal
		 // incidence is impossible to be of type 4 because there is no cure.
		 else if (dsmeta[j].mrtl.type == "Type4")
		 {
			for (vector<IntegerVector>::size_type k = 0; k < dsmeta[j].mrtl.influenced_by.disease_prvl.size(); ++k) // Loop over influenced by diseases
			{
			  if (dsmeta[j].incd.influenced_by.lag[k] > 0 && // to exclude conditions that depend on themselves, like asthma
					VECT_ELEM(dsmeta[j].mrtl.influenced_by.disease_prvl[k],i - dsmeta[j].mrtl.influenced_by.lag[k]) > 0)
			  {
				 mltp *= dsmeta[j].mrtl.influenced_by.mltp[k](i); // no lag here
			  }
			}

			if (VECT_ELEM(dsmeta[j].incd.prvl,i) <= (dsmeta[j].dgns.flag ? dsmeta[j].dgns.cure : dsmeta[j].mrtl.cure)) // Valid since not universal incidence
			{
			  if (dsmeta[j].mrtl1flag)  // if fatality for incident cases estimated separately
			  {
				 if (VECT_ELEM(dsmeta[j].incd.prvl,i) == 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl1,i))        tempdead.push_back(dsmeta[j].mrtl.death_code); // mltp not needed here
				 if (VECT_ELEM(dsmeta[j].incd.prvl,i)  > 1 && rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i) * mltp) tempdead.push_back(dsmeta[j].mrtl.death_code);
			  }
			  else if (rn1 < VECT_ELEM(dsmeta[j].mrtl.prbl2,i) * mltp) tempdead.push_back(dsmeta[j].mrtl.death_code);
			}
			else dsmeta[j].mrtl.flag = true; // if alive cure after defined period

			mltp = 1.0;
		 } // End Type 4 mortality
	  } // end if prevalent case
	}
	catch(std::exception &e) { 
		// forward standard library exceptions as runtime_errors - all these have a explanatory message which Rcpp prints.
		throw std::runtime_error((string)"Error within EvalMortality(): "+e.what());
	}
}

//' @export
// [[Rcpp::export]]
void simcpp(DataFrame dt, const List l, const int mc)
{
	try { // catch all simcpp() exceptions
	
  uint64_t seed = rng->operator()();
  rng =  dqrng::generator<pcg64>(seed);
  using parm_t = decltype(uniform)::param_type;
  uniform.param(parm_t(0.0, 1.0));

  uint64_t _stream = 0;
  uint64_t _seed = 0;

  const int n = dt.nrow(), SIMULANT_ALIVE=0;

  simul_meta meta = get_simul_meta(l, dt);

  List diseases = l["diseases"];
  const int dn = diseases.length();
  vector<disease_meta> dsmeta(dn);
  for (int j = 0; j < dn; ++j) dsmeta[j] = get_disease_meta(diseases[j], dt);


  double mltp = 1.0; // to be used for influence_by multiplier
  vector<int> tempdead; // temporary dead possibly from multiple causes
  double rn1, rn2;
  int pid_buffer = meta.pid[0]; // flag for when new pid to reset other flags. Holds the last pid
  bool pid_mrk = true;

  for (int i = 0; i < n; ++i) // loop over dt rows (and resolve all diseases for each row before you move on)
  {
    // set true when new participant (need to be here and remain true until the
    // next if
    if ((i==0 || meta.dead[i-1]==SIMULANT_ALIVE) && meta.year[i] >= meta.init_year && meta.age[i] >= meta.age_low)
    {
      // NOTE values of i - x are certainly inbound and belong to the same
      // individual as long as x <= max_lag
      // NA_INTEGER == 0 returns false

      if (i>0 && meta.pid[i] == pid_buffer) // same simulant as the last row?
      {
        pid_mrk = false;
      }
      else {
        pid_mrk = true;
        pid_buffer = meta.pid[i];
      }

      _seed = u32tou64(meta.pid[i], meta.year[i]);

      for (int j = 0; j < dn; ++j) // loop over diseases
      {
        _stream = u32tou64(mc, dsmeta[j].seed);
        rng->seed(_seed, _stream);

        // Incidence ------------------------------------------------
        // Generate RN irrespective of whether will be used. Crucial for
        // reproducibility and to remove stochastic noise between scenarios
        rn1 = runif_impl();
        // reset flags for new simulants
        if (pid_mrk)
        {
          dsmeta[j].incd.flag = (dsmeta[j].incd.type == "Universal" || VECT_ELEM(dsmeta[j].incd.prvl,i) > 0) ? true : false; // denotes that incd occurred, always true for universal that doesn't have dsmeta[j].incd.prvl(i)
          dsmeta[j].mrtl.flag = false; // denotes cure
        }


        // reset prvl if cure. Always false for new patient. No issue with init
        // year as logic makes sense for diseases, even for asthma. The flag
        // signifies cure that happened at some point in the previous year and
        // reflects at the beginning of current year.
        if (dsmeta[j].mrtl.flag)
        {
			 VECT_ELEM(dsmeta[j].incd.prvl,i) = 0;
          VECT_ELEM(dsmeta[j].dgns.prvl,i) = 0;
          if ((dsmeta[j].dgns.type == "Type0" || dsmeta[j].dgns.type == "Type1") && dsmeta[j].dgns.mm_wt > 0.0 && VECT_ELEM(dsmeta[j].dgns.prvl,i - 1) > 0) {
					meta.mm_score[i] -= dsmeta[j].dgns.mm_wt;
					meta.mm_count[i]--;
			 }

          dsmeta[j].mrtl.flag = false;
        }

        EvalDiseaseIncidence(dsmeta,i, j, rn1, mltp, meta);
        EvalDiagnosis(dsmeta,rn1, j, i, meta);
        EvalMortality(dsmeta,tempdead,i, j, rn1, mltp);
      } // end loop over diseases


      // resolve mortality from multiple causes in a year



      if (tempdead.size() == 1) meta.dead[i] = tempdead[0];
      if (tempdead.size() > 1)
      {
        _stream = u32tou64(mc, 1L);
        rng->seed(_seed, _stream);
        rn2 = runif_impl(); // the seed for this row defined for each mc. So is is safe/reproducible to only be calculated when needed.

        int ind = (int)(rn2 * 100000000) % tempdead.size();
        meta.dead[i] = tempdead[ind];
      }
      tempdead.clear();

    } // end year >= init_year & alive

	 // for subsequent rows, carry forward the 'dead' label for dead simulants.
	 // labels {0; >0; NA} indicate {alive; cause of death (COD); longdead} simulants.
	 //REFACTOR(mbirkett): could simply replace below if(...) statement with else {..}, as above if(.. SIMULANT_ALIVE ..) 
		 // handles all other cases; assuming: meta.dead[i]<0 always true, and above brace doesnt change anything relevant to below.
	 if (i>0 && (meta.dead[i - 1]>SIMULANT_ALIVE || IntegerVector::is_na(meta.dead[i - 1])) &&
        meta.year[i] >= meta.init_year && meta.age[i] >= meta.age_low)
      meta.dead[i] = NA_INTEGER; // NA_INTEGER > 0 is false
			//CHECK(mbirkett): why does last assign NA_INTEGER when above states label carried forward?

		} // end: loop over dt rows
	} // end: catch all simcpp() exceptions

	// handler to catch all simcpp() exceptions
	catch(std::exception &e) { 
		// forward standard library exceptions as runtime_errors - all these have a explanatory message which Rcpp prints.
		throw std::runtime_error(__FILE__+(string)": "+e.what());
	}
	catch(...) { // other exceptions may not have a suitable message (may even be difficult to identify that this file is the origin).
		throw std::runtime_error((string)"Exception within "+__FILE__+(string)"; "+
			"on investigating, it may helpful to disable this catch(...){} statement.");
	}
}
