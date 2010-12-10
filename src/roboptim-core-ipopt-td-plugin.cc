// Copyright (C) 2009 by Thomas Moulard, AIST, CNRS, INRIA.
//
// This file is part of the roboptim.
//
// roboptim is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// roboptim is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with roboptim.  If not, see <http://www.gnu.org/licenses/>.

#include <cassert>
#include <cstring>

#include <boost/foreach.hpp>
#include <boost/variant/apply_visitor.hpp>

#include <coin/IpIpoptApplication.hpp>
#include <coin/IpTNLP.hpp>

#include <roboptim/core/util.hh>

#include "roboptim/core/plugin/ipopt-td.hh"


namespace roboptim
{
  using namespace Ipopt;

  namespace detail
  {
    TNLP::LinearityType cfsqp_tag (const LinearFunction& f);
    TNLP::LinearityType cfsqp_tag (const Function& f);

    void
    jacobianFromGradients
    (DerivableFunction::matrix_t& jac,
     const IpoptSolverTd::problem_t::constraints_t& c,
     const TwiceDerivableFunction::vector_t& x);

    /// \internal
    /// Set "linear" tag to linear functions.
    TNLP::LinearityType cfsqp_tag (const LinearFunction&)
    {
      return TNLP::LINEAR;
    }

    /// \internal
    /// Set "non_linear" tag to non linear functions.
    TNLP::LinearityType cfsqp_tag (const Function&)
    {
      return TNLP::NON_LINEAR;
    }

    /// \internal
    /// Concatenate jacobians.
    void
    jacobianFromGradients
    (DerivableFunction::matrix_t& jac,
     const IpoptSolverTd::problem_t::constraints_t& c,
     const TwiceDerivableFunction::vector_t& x)
    {
      using namespace boost;
      for (unsigned i = 0; i < jac.size1 (); ++i)
	{
	  shared_ptr<TwiceDerivableFunction> g =
	    get<shared_ptr<TwiceDerivableFunction> > (c[i]);
	  DerivableFunction::jacobian_t grad = g->jacobian (x);

	  for (unsigned j = 0; j < jac.size2 (); ++j)
	    jac (i, j) = grad(0, j);
	}
    }



    /// \internal
    /// Ipopt non linear problem definition.
    struct TnlpTd : public TNLP
    {
      TnlpTd (IpoptSolverTd& solver)
        throw ()
        : solver_ (solver)
      {}

      virtual bool
      get_nlp_info (Index& n, Index& m, Index& nnz_jac_g,
                    Index& nnz_h_lag, TNLP::IndexStyleEnum& index_style)
        throw ()
      {
        n = solver_.problem ().function ().inputSize ();
        m = solver_.problem ().constraints ().size ();
        nnz_jac_g = n * m; //FIXME: use a dense matrix for now.
        nnz_h_lag = n * n; //FIXME: use a dense matrix for now.
        index_style = TNLP::C_STYLE;
        return true;
      }

      virtual bool
      get_bounds_info (Index n, Number* x_l, Number* x_u,
                       Index m, Number* g_l, Number* g_u)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        typedef IpoptSolverTd::problem_t::intervals_t::const_iterator citer_t;
        for (citer_t it = solver_.problem ().argumentBounds ().begin ();
             it != solver_.problem ().argumentBounds ().end (); ++it)
          *(x_l++) = (*it).first, *(x_u++) = (*it).second;

        for (citer_t it = solver_.problem ().bounds ().begin ();
             it != solver_.problem ().bounds ().end (); ++it)
          *(g_l++) = (*it).first, *(g_u++) = (*it).second;
        return true;
      }

      virtual bool
      get_scaling_parameters (Number&,
                              bool& use_x_scaling, Index n,
                              Number* x_scaling,
                              bool& use_g_scaling, Index m,
                              Number* g_scaling)
        throw ()
      {
        use_x_scaling = true, use_g_scaling = true;

        memcpy (x_scaling, &solver_.problem ().argumentScales ()[0],
                n * sizeof (double));

        for (Index i = 0; i < m; ++i)
          g_scaling[i] = solver_.problem ().scales ()[i];
        return true;
      }

      virtual bool
      get_variables_linearity (Index n, LinearityType* var_types) throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);

        //FIXME: detect from problem.
        for (Index i = 0; i < n; ++i)
          var_types[i] = cfsqp_tag (solver_.problem ().function ());
        return true;
      }

      virtual bool
      get_function_linearity (Index m, LinearityType* const_types) throw ()
      {
	using namespace boost;
        assert (solver_.problem ().constraints ().size () - m == 0);

        for (Index i = 0; i < m; ++i)
	  {
	    shared_ptr<TwiceDerivableFunction> f =
	      get<shared_ptr<TwiceDerivableFunction> >
	      (solver_.problem ().constraints ()[i]);
	    const_types[i] = cfsqp_tag (*f);
	  }
        return true;
      }

      virtual bool
      get_starting_point (Index n, bool init_x, Number* x,
                          bool init_z, Number* z_L, Number* z_U,
                          Index m, bool init_lambda,
                          Number*)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        //FIXME: handle all modes.
        assert(init_lambda == false);

        // Set bound multipliers.
        if (init_z)
          {
            //FIXME: for now, if required, scale is one.
            //When do we need something else?
            for (Index i = 0; i < n; ++i)
              z_L[i] = 1., z_U[i] = 1.;
          }

        // Set the starting point.
        if (!solver_.problem ().startingPoint () && init_x)
          {
            solver_.result_ =
              SolverError ("Ipopt method needs a starting point.");
            return false;
          }
        if (!solver_.problem ().startingPoint ())
          return true;

        vector_to_array (x, *solver_.problem ().startingPoint ());
        return true;
      }

      virtual bool
      get_warm_start_iterate (IteratesVector&) throw ()
      {
        //FIXME: implement this.
        //IteratesVector is defined in src/Algorithm/IteratesVector.hpp
        //and is not distributed (not a distributed header).
        //Hence, seems difficult to manipulate this type.
        //Idea 1: offer the possibility either to retrive this data after
        //solving a problem.
        //Idea 2: creating this type manually from problem + rough guess
        //(or previous solution).
        return false;
      }

      virtual bool
      eval_f (Index n, const Number* x, bool, Number& obj_value)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);

        IpoptSolverTd::vector_t x_ (n);
        array_to_vector (x_, x);
        obj_value = solver_.problem ().function () (x_)[0];
        return true;
      }

      virtual bool
      eval_grad_f (Index n, const Number* x, bool, Number* grad_f)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);

        IpoptSolverTd::vector_t x_ (n);
        array_to_vector (x_, x);

        Function::vector_t grad =
          solver_.problem ().function ().gradient (x_, 0);
        vector_to_array(grad_f, grad);
        return true;
      }

      virtual bool
      eval_g (Index n, const Number* x, bool,
              Index m, Number* g)
        throw ()
      {
	using namespace boost;
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        IpoptSolverTd::vector_t x_ (n);
        array_to_vector (x_, x);

        typedef IpoptSolverTd::problem_t::constraints_t::const_iterator citer_t;

        IpoptSolverTd::vector_t g_ (m);
        int i = 0;
        for (citer_t it = solver_.problem ().constraints ().begin ();
             it != solver_.problem ().constraints ().end (); ++it, ++i)
	  {
	    shared_ptr<TwiceDerivableFunction> g =
	      get<shared_ptr<TwiceDerivableFunction> > (*it);
	    g_[i] = (*g) (x_)[0];
	  }
        vector_to_array(g, g_);
        return true;
      }

      virtual bool
      eval_jac_g(Index n, const Number* x, bool,
                 Index m, Index, Index* iRow,
                 Index *jCol, Number* values)
        throw ()
      {
	using namespace boost;
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        if (!values)
          {
            //FIXME: always dense for now.
            int idx = 0;
            for (int i = 0; i < m; ++i)
              for (int j = 0; j < n; ++j)
                {
                  iRow[idx] = i, jCol[idx] = j;
                  ++idx;
                }
          }
        else
          {
            IpoptSolverTd::vector_t x_ (n);
            array_to_vector (x_, x);
            Function::matrix_t jac
	      (solver_.problem ().constraints ().size (),
	       solver_.problem ().function ().inputSize ());
            jacobianFromGradients (jac, solver_.problem ().constraints (), x_);
            int idx = 0;
            for (int i = 0; i < m; ++i)
	      for (int j = 0; j < n; ++j)
		values[idx++] = jac (i, j);
          }

        return true;
      }

      /// Compute Ipopt hessian from several hessians.
      void compute_hessian (TwiceDerivableFunction::hessian_t& h,
                            const IpoptSolverTd::vector_t& x,
                            Number obj_factor,
                            const Number* lambda)
        throw ()
      {
        typedef IpoptSolverTd::problem_t::constraints_t::const_iterator citer_t;

        TwiceDerivableFunction::hessian_t fct_h =
          solver_.problem ().function ().hessian (x, 0);
        h = obj_factor * fct_h;

        int i = 0;
        for (citer_t it = solver_.problem ().constraints ().begin ();
             it != solver_.problem ().constraints ().end (); ++it)
	  {
	    shared_ptr<TwiceDerivableFunction> g =
	      get<shared_ptr<TwiceDerivableFunction> > (*it);
	    h += lambda[i++] * g->hessian (x, 0);
	  }
      }

      virtual bool
      eval_h (Index n, const Number* x, bool,
              Number obj_factor, Index m, const Number* lambda,
              bool, Index nele_hess, Index* iRow,
              Index* jCol, Number* values)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        //FIXME: check if a hessian is provided.

        if (!values)
          {
            //FIXME: always dense for now.
            int idx = 0;
            for (int i = 0; i < n; ++i)
              for (int j = 0; j < n; ++j)
                {
                  iRow[idx] = i, jCol[idx] = j;
                  ++idx;
                }
            assert (idx == nele_hess);
          }
        else
          {
            IpoptSolverTd::vector_t x_ (n);
            array_to_vector (x_, x);

            TwiceDerivableFunction::hessian_t h
	      (solver_.problem ().function ().inputSize (),
	       solver_.problem ().function ().inputSize ());
            compute_hessian (h, x_, obj_factor, lambda);

            int idx = 0;
            for (int i = 0; i < n; ++i)
              for (int j = 0; j < n; ++j)
                values[idx++] = h (i, j);
            assert (idx == nele_hess);
          }

        return true;
      }


#define SWITCH_ERROR(NAME, ERROR)		\
      case NAME:				\
      solver_.result_ = SolverError (ERROR);	\
      break

#define SWITCH_FATAL(NAME)			\
      case NAME:				\
      assert (0);				\
      break

#define MAP_IPOPT_ERRORS(MACRO)						\
      MACRO(MAXITER_EXCEEDED, "Max iteration exceeded");		\
      MACRO(STOP_AT_TINY_STEP,						\
	    "Algorithm proceeds with very little progress");		\
      MACRO(LOCAL_INFEASIBILITY,					\
	    "Algorithm converged to a point of local infeasibility");	\
      MACRO(DIVERGING_ITERATES, "Iterate diverges");			\
      MACRO(RESTORATION_FAILURE, "Restoration phase failed");		\
      MACRO(ERROR_IN_STEP_COMPUTATION,					\
	    "Unrecoverable error while Ipopt tried to compute"		\
	    " the search direction");					\
      MACRO(INVALID_NUMBER_DETECTED,					\
	    "Ipopt received an invalid number");			\
      MACRO(INTERNAL_ERROR, "Unknown internal error");			\
      MACRO(TOO_FEW_DEGREES_OF_FREEDOM, "Two few degrees of freedom");	\
      MACRO(INVALID_OPTION, "Invalid option");				\
      MACRO(OUT_OF_MEMORY, "Out of memory");				\
      MACRO(CPUTIME_EXCEEDED, "Cpu time exceeded")


#define MAP_IPOPT_FATALS(MACRO)						\
      MACRO(USER_REQUESTED_STOP)

#define FILL_RESULT()					\
      array_to_vector (res.x, x);			\
      res.constraints.resize (m);			\
      array_to_vector (res.constraints, g);		\
      res.lambda.resize (m);				\
      array_to_vector (res.lambda, lambda);		\
      res.value (0) = obj_value


      virtual void
      finalize_solution(SolverReturn status,
                        Index n, const Number* x, const Number*,
                        const Number*, Index m, const Number* g,
                        const Number* lambda, Number obj_value,
                        const IpoptData*,
                        IpoptCalculatedQuantities*)
        throw ()
      {
        assert (solver_.problem ().function ().inputSize () - n == 0);
        assert (solver_.problem ().constraints ().size () - m == 0);

        switch (status)
          {
	  case FEASIBLE_POINT_FOUND:
          case SUCCESS:
            {
              Result res (n, 1);
	      FILL_RESULT ();
              solver_.result_ = res;
              break;
            }
          case STOP_AT_ACCEPTABLE_POINT:
            {
              ResultWithWarnings res (n, 1);
	      FILL_RESULT ();
              res.warnings.push_back (SolverWarning ("Acceptable point"));
              solver_.result_ = res;
              break;
            }

	    MAP_IPOPT_ERRORS(SWITCH_ERROR);
	    MAP_IPOPT_FATALS(SWITCH_FATAL);
	  }
	assert (solver_.result_.which () != IpoptSolverTd::SOLVER_NO_SOLUTION);
      }

#undef FILL_RESULT
#undef SWITCH_ERROR
#undef SWITCH_FATAL
#undef MAP_IPOPT_ERRORS
#undef MAP_IPOPT_FATALS


      virtual bool
      intermediate_callback (AlgorithmMode,
                             Index, Number,
                             Number, Number,
                             Number, Number,
                             Number,
                             Number, Number,
                             Index,
                             const IpoptData*,
                             IpoptCalculatedQuantities*)
        throw ()
      {
        return true;
      }

      virtual Index
      get_number_of_nonlinear_variables () throw ()
      {
        //FIXME: implement this.
        return -1;
      }

      virtual bool
      get_list_of_nonlinear_variables (Index,
                                       Index*)
        throw ()
      {
        //FIXME: implement this.
        return false;
      }

      IpoptSolverTd& solver_;
    };
  } // end of namespace detail

  using namespace detail;


  // On Microsoft Windows, working with pre-built Ipopt
  // binaries requires the use of the IpoptApplicationFactory.
  IpoptSolverTd::IpoptSolverTd (const problem_t& pb) throw ()
    : parent_t (pb),
      nlp_ (new TnlpTd (*this)),
      app_ (IpoptApplicationFactory ())
  {
    app_->Jnlst()->DeleteAllJournals();
    // Initialize parameters.
    initializeParameters ();
  }

#define DEFINE_PARAMETER(KEY, DESCRIPTION, VALUE)	\
  do {							\
    parameters_[KEY].description = DESCRIPTION;		\
    parameters_[KEY].value = VALUE;			\
  } while (0)

  void
  IpoptSolverTd::initializeParameters () throw ()
  {
    parameters_.clear ();

    // Shared parameters.
    DEFINE_PARAMETER ("max-iterations", "number of iterations", 3000);

    // IPOPT specific.
    // Much more options are available for Ipopt, see ``Options reference'' of
    // Ipopt documentation.

    //  Output
    DEFINE_PARAMETER ("ipopt.print_level", "output verbosity level", 5);
    DEFINE_PARAMETER ("ipopt.print_user_options",
		      "print all options set by the user", "no");
    DEFINE_PARAMETER ("ipopt.print_options_documentation",
		      "switch to print all algorithmic options", "no");
    DEFINE_PARAMETER
      ("ipopt.output_file",
       "file name of desired output file (leave unset for no file output)", "");
    DEFINE_PARAMETER ("ipopt.file_print_level",
		      "verbosity level for output file", 5);
    DEFINE_PARAMETER ("ipopt.option_file_name",
		      "file name of options file (to overwrite default)", "");

    //  Termination
    DEFINE_PARAMETER ("ipopt.tol",
		      "desired convergence tolerance (relative)", 1e-7);

    //  Barrier parameter
    DEFINE_PARAMETER ("ipopt.mu_strategy",
		      "update strategy for barrier parameter", "adaptive");

  }

#undef DEFINE_PARAMETER


  namespace
  {
    struct IpoptParametersUpdater
      : public boost::static_visitor<>
    {
      explicit IpoptParametersUpdater
      (const Ipopt::SmartPtr<Ipopt::IpoptApplication>& app,
       const std::string& key)
	: app (app),
	  key (key)

      {}
      void
      operator () (const Function::value_type& val) const
      {
	app->Options ()->SetNumericValue (key, val);
      }

      void
      operator () (const int& val) const
      {
	app->Options ()->SetNumericValue (key, val);
      }

      void
      operator () (const std::string& val) const
      {
	app->Options ()->SetStringValue (key, val);
      }

    private:
      const Ipopt::SmartPtr<Ipopt::IpoptApplication>& app;
      const std::string& key;
    };
  } // end of anonymous namespace.

  void
  IpoptSolverTd::updateParameters () throw ()
  {
    const std::string prefix = "ipopt.";
    typedef const std::pair<const std::string, Parameter> const_iterator_t;
    BOOST_FOREACH (const_iterator_t& it, parameters_)
      {
	if (it.first.substr (0, prefix.size ()) == prefix)
	  {
	    boost::apply_visitor
	      (IpoptParametersUpdater
	       (app_, it.first.substr (prefix.size ())), it.second.value);
	  }
      }

    // Remap standardized parameters.
    boost::apply_visitor
      (IpoptParametersUpdater
       (app_, "max_iter"), parameters_["max-iterations"].value);
  }

  IpoptSolverTd::~IpoptSolverTd () throw ()
  {
  }


#define SWITCH_ERROR(NAME, ERROR)		\
  case NAME:					\
  result_ = SolverError (ERROR);		\
  break

#define SWITCH_FATAL(NAME)			\
  case NAME:					\
  assert (0);					\
  break

#define SWITCH_OK(NAME, CASES)			\
  case NAME:					\
  {						\
    int status = app_->OptimizeTNLP (nlp_);	\
    switch (status)				\
      {						\
	CASES;					\
      }						\
  }						\
  break

#define MAP_IPOPT_ERRORS(MACRO)						\
  MACRO (Infeasible_Problem_Detected,					\
	 "Infeasible problem detected");				\
  MACRO (Search_Direction_Becomes_Too_Small,				\
	 "Search direction too small");					\
  MACRO (Diverging_Iterates, "Diverging iterates");			\
  MACRO (Maximum_Iterations_Exceeded,					\
	 "Maximum iterations exceeded");				\
  MACRO (Restoration_Failed, "Restoration failed");			\
  MACRO (Error_In_Step_Computation, "Error in step computation");	\
  MACRO (Not_Enough_Degrees_Of_Freedom,					\
	 "Not enough degrees of freedom");				\
  MACRO (Invalid_Problem_Definition,					\
	 "Invalid problem definition");					\
  MACRO (Invalid_Option, "Invalid option");				\
  MACRO (Invalid_Number_Detected, "Invalid number detected");		\
  MACRO (Unrecoverable_Exception, "Unrecoverable exception");		\
  MACRO (Insufficient_Memory, "Insufficient memory");			\
  MACRO (Internal_Error, "Internal error");				\
  MACRO (Maximum_CpuTime_Exceeded, "Maximum CPU time exceeded")

#define MAP_IPOPT_FATALS(MACRO)			\
  MACRO(User_Requested_Stop);			\
  MACRO(NonIpopt_Exception_Thrown)

#define MAP_IPOPT_OKS(MACRO)						\
  MACRO (Solve_Succeeded, MAP_IPOPT_ERRORS(SWITCH_ERROR);		\
	 MAP_IPOPT_FATALS(SWITCH_FATAL));				\
  MACRO (Solved_To_Acceptable_Level,MAP_IPOPT_ERRORS(SWITCH_ERROR);	\
	 MAP_IPOPT_FATALS(SWITCH_FATAL));				\
  MACRO (Feasible_Point_Found,MAP_IPOPT_ERRORS(SWITCH_ERROR);		\
	 MAP_IPOPT_FATALS(SWITCH_FATAL))

  void
  IpoptSolverTd::solve () throw ()
  {
    // Read parameters and forward them to Ipopt.
    updateParameters ();
    ApplicationReturnStatus status = app_->Initialize ("");

    switch (status)
      {
	MAP_IPOPT_OKS (SWITCH_OK);
	MAP_IPOPT_ERRORS (SWITCH_ERROR);
	MAP_IPOPT_FATALS (SWITCH_FATAL);
      }
    assert (result_.which () != SOLVER_NO_SOLUTION);
  }

#undef SWITCH_ERROR
#undef SWITCH_FATAL
#undef SWITCH_OK
#undef MAP_IPOPT_ERRORS
#undef MAP_IPOPT_FATALS
#undef MAP_IPOPT_OKS


  Ipopt::SmartPtr<Ipopt::IpoptApplication>
  IpoptSolverTd::getIpoptApplication () throw ()
  {
    return app_;
  }

} // end of namespace roboptim

extern "C"
{
  using namespace roboptim;
  typedef IpoptSolverTd::parent_t solver_t;

  ROBOPTIM_DLLEXPORT unsigned getSizeOfProblem ();
  ROBOPTIM_DLLEXPORT solver_t* create (const IpoptSolverTd::problem_t& pb);
  ROBOPTIM_DLLEXPORT void destroy (solver_t* p);

  ROBOPTIM_DLLEXPORT unsigned getSizeOfProblem ()
  {
    return sizeof (IpoptSolverTd::problem_t);
  }

  ROBOPTIM_DLLEXPORT solver_t* create (const IpoptSolverTd::problem_t& pb)
  {
    return new IpoptSolverTd (pb);
  }

  ROBOPTIM_DLLEXPORT void destroy (solver_t* p)
  {
    delete p;
  }
}


// Local Variables:
// compile-command: "cd ../_build && make -k"
// End: