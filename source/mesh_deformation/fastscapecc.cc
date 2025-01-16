/*
  Copyright (C) 2011 - 2022 by the authors of the ASPECT code.

  This file is part of ASPECT.

  ASPECT is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  ASPECT is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ASPECT; see the file LICENSE.  If not see
  <http://www.gnu.org/licenses/>.
 */


#include <iostream>
#include <aspect/global.h>

#include <aspect/mesh_deformation/fastscapecc.h>
#include <aspect/geometry_model/box.h>
#include <deal.II/numerics/vector_tools.h>
#include <aspect/postprocess/visualization.h>
#include <ctime>
#include <aspect/simulator.h>
#include <aspect/simulator/assemblers/interface.h>
#include <aspect/simulator_signals.h>

#include <memory>
#include <algorithm> // For std::copy

#include <aspect/geometry_model/spherical_shell.h>
#include <fastscapelib/grid/healpix_grid.hpp>
#include <aspect/mesh_deformation/interface.h>

// namespace fs = fastscapelib;


namespace aspect
{
  namespace MeshDeformation
  {
    template <int dim>
    void FastScapecc<dim>::initialize ()
    {
      const GeometryModel::Box<dim> *box_geometry
        = dynamic_cast<const GeometryModel::Box<dim>*>(&this->get_geometry_model());

      const GeometryModel::SphericalShell<dim> *spherical_geometry
        = dynamic_cast<const GeometryModel::SphericalShell<dim>*>(&this->get_geometry_model());

      if (geometry_type == GeometryType::Box)
        {
          this->get_pcout() << "Box geometry detected. Initializing FastScape for Box geometry..." << std::endl;

          grid_extent[0].first = box_geometry->get_origin()[0];
          grid_extent[0].second = box_geometry->get_extents()[0];
          grid_extent[1].first = box_geometry->get_origin()[1];
          grid_extent[1].second = box_geometry->get_extents()[1];

          nx = repetitions[0] + 1;
          dx = (grid_extent[0].second) / repetitions[0];

          ny = repetitions[1] + 1;
          dy = (grid_extent[1].second) / repetitions[1];

          x_extent = grid_extent[0].second;
          y_extent = grid_extent[1].second;
          array_size = nx * ny;
        }
      else if (geometry_type == GeometryType::SphericalShell)
        {
          this->get_pcout() << "Spherical Shell geometry detected. Initializing FastScape for Spherical Shell geometry..." << std::endl;

          nsides = static_cast<int>(sqrt(48 * std::pow(2, (additional_refinement_levels + surface_refinement_difference + maximum_surface_refinement_level) * 2) / 12));
        }
      else
        {
          AssertThrow(false, ExcMessage("FastScapecc plugin only supports Box or Spherical Shell geometries."));
        }
    }

    template <int dim>
    void
    FastScapecc<dim>::compute_velocity_constraints_on_boundary(const DoFHandler<dim> &mesh_deformation_dof_handler,
                                                               AffineConstraints<double> &mesh_velocity_constraints,
                                                               const std::set<types::boundary_id> &boundary_ids) const
    {
      if (this->get_timestep_number() == 0)
        return;

      TimerOutput::Scope timer_section(this->get_computing_timer(), "FastScape plugin");

      const unsigned int current_timestep = this->get_timestep_number ();

      const types::boundary_id relevant_boundary = this->get_geometry_model().translate_symbolic_boundary_name_to_id ("top");
      std::vector<std::vector<double>> temporary_variables(dim + 2, std::vector<double>());

      // Get a quadrature rule that exists only on the corners, and increase the refinement if specified.
      const QIterated<dim-1> face_corners (QTrapezoid<1>(),
                                           static_cast<unsigned int>(std::pow(2, additional_refinement_levels + surface_refinement_difference)));

      FEFaceValues<dim> fe_face_values (this->get_mapping(),
                                        this->get_fe(),
                                        face_corners,
                                        update_values |
                                        update_quadrature_points |
                                        update_normal_vectors);

      auto healpix_grid = T_Healpix_Base<int>(nsides, Healpix_Ordering_Scheme::RING, SET_NSIDE);

      for (const auto &cell : this->get_dof_handler().active_cell_iterators())
        if (cell->is_locally_owned() && cell->at_boundary())
          for (unsigned int face_no = 0; face_no < GeometryInfo<dim>::faces_per_cell; ++face_no)
            if (cell->face(face_no)->at_boundary())
              {
                if (cell->face(face_no)->boundary_id() != relevant_boundary)
                  continue;

                std::vector<Tensor<1,dim>> vel(face_corners.size());
                fe_face_values.reinit(cell, face_no);
                fe_face_values[this->introspection().extractors.velocities].get_function_values(this->get_solution(), vel);

                for (unsigned int corner = 0; corner < face_corners.size(); ++corner)
                  {
                    const Point<dim> vertex = fe_face_values.quadrature_point(corner);

                    // Find the healpix index for the current point.
                    int index = healpix_grid.vec2pix({vertex(0), vertex(1), vertex(2)});

                    // Convert Cartesian velocity to radial velocity.
                    double radial_velocity = (vertex(0) * vel[corner][0] + vertex(1) * vel[corner][1] + vertex(2) * vel[corner][2])
                                             / vertex.norm();

                    temporary_variables[0].push_back(vertex(dim-1) - outer_radius);   // z component
                    temporary_variables[1].push_back(index);
                    temporary_variables[2].push_back(radial_velocity * year_in_seconds);
                  }
              }

      int array_size = healpix_grid.Npix();
      std::vector<double> V(array_size);

      if (Utilities::MPI::this_mpi_process(this->get_mpi_communicator()) == 0)
        {
          // Initialize the variables that will be sent to FastScape.
          std::vector<double> h(array_size, std::numeric_limits<double>::max());
          std::vector<double> vz(array_size);
          std::vector<double> h_old(array_size);

          for (unsigned int i = 0; i < temporary_variables[1].size(); ++i)
            {
              int index = static_cast<int>(temporary_variables[1][i]);
              h[index] = temporary_variables[0][i];
              vz[index] = temporary_variables[2][i];
            }

          for (unsigned int p = 1; p < Utilities::MPI::n_mpi_processes(this->get_mpi_communicator()); ++p)
            {
              MPI_Status status;
              MPI_Probe(p, 42, this->get_mpi_communicator(), &status);
              int incoming_size = 0;
              MPI_Get_count(&status, MPI_DOUBLE, &incoming_size);

              for (unsigned int i = 0; i < temporary_variables.size(); ++i)
                {
                  temporary_variables[i].resize(incoming_size);
                }

              for (unsigned int i = 0; i < temporary_variables.size(); ++i)
                MPI_Recv(&temporary_variables[i][0], incoming_size, MPI_DOUBLE, p, 42, this->get_mpi_communicator(), &status);

              for (unsigned int i = 0; i < temporary_variables[1].size(); ++i)
                {
                  int index = static_cast<int>(temporary_variables[1][i]);
                  h[index] = temporary_variables[0][i];
                  vz[index] = temporary_variables[2][i];
                }
            }

          for (unsigned int i = 0; i < array_size; ++i)
            {
              h_old[i] = h[i];
            }

          const double aspect_timestep_in_years = this->get_timestep() / year_in_seconds;

          unsigned int fastscape_iterations = fastscape_steps_per_aspect_step;
          double fastscape_timestep_in_years = aspect_timestep_in_years / fastscape_iterations;
          while (fastscape_timestep_in_years > maximum_fastscape_timestep)
            {
              fastscape_iterations *= 2;
              fastscape_timestep_in_years *= 0.5;
            }

          xt::xarray<fastscapelib::node_status> node_status_array = xt::zeros<fastscapelib::node_status>({ array_size });
          auto grid = fastscapelib::healpix_grid<>(nsides, node_status_array, 6.371e6);
          auto flow_graph = fastscapelib::flow_graph<fastscapelib::healpix_grid<>>(
                              grid,
          {
            fastscapelib::single_flow_router()
          }
                            );
          auto spl_eroder = fastscapelib::make_spl_eroder(flow_graph, 2e-4, 0.4, 1, 1e-5);

          xt::xarray<double> uplifted_elevation = xt::zeros<double>(grid.shape());
          xt::xarray<double> drainage_area = xt::zeros<double>(grid.shape());
          xt::xarray<double> sediment_flux = xt::zeros<double>(grid.shape());

          std::vector<std::size_t> shape = { static_cast<unsigned long>(array_size) };
          auto uplift_rate = xt::adapt(vz, shape);
          auto elevation = xt::adapt(h, shape);
          auto elevation_old = xt::adapt(h_old, shape);

          for (unsigned int fastscape_iteration = 0; fastscape_iteration < fastscape_iterations; ++fastscape_iteration)
            {
              uplifted_elevation = elevation + fastscape_timestep_in_years * uplift_rate;
              flow_graph.update_routes(uplifted_elevation);
              flow_graph.accumulate(drainage_area, 1.0);
              auto spl_erosion = spl_eroder.erode(uplifted_elevation, drainage_area, fastscape_timestep_in_years);
              sediment_flux = flow_graph.accumulate(spl_erosion);
              elevation = uplifted_elevation - spl_erosion;
            }

          std::vector<double> elevation_std(elevation.begin(), elevation.end());
          std::vector<double> elevation_old_std(elevation_old.begin(), elevation.end());

          for (unsigned int i = 0; i < array_size; ++i)
            {
              V[i] = (elevation_std[i] - elevation_old_std[i]) / (this->get_timestep() / year_in_seconds);
            }
          MPI_Bcast(&V[0], array_size, MPI_DOUBLE, 0, this->get_mpi_communicator());
        }
      else
        {
          for (unsigned int i = 0; i < temporary_variables.size(); ++i)
            MPI_Ssend(&temporary_variables[i][0], temporary_variables[1].size(), MPI_DOUBLE, 0, 42, this->get_mpi_communicator());

          MPI_Bcast(&V[0], array_size, MPI_DOUBLE, 0, this->get_mpi_communicator());
        }

      auto healpix_velocity_function = [&](const Point<dim> &p) -> double
      {
        int index = healpix_grid.vec2pix({p(0), p(1), p(2)});
        return V[index];
      };

      VectorFunctionFromScalarFunctionObject<dim> vector_function_object(
        healpix_velocity_function,
        dim - 1,
        dim);

      VectorTools::interpolate_boundary_values (mesh_deformation_dof_handler,
                                                *boundary_ids.begin(),
                                                vector_function_object,
                                                mesh_velocity_constraints);
    }


    template <int dim>
    Table<dim,double>
    FastScapecc<dim>::fill_data_table(std::vector<double> &values,
                                      TableIndices<dim> &size_idx,
                                      const int &array_size) const
    {
      // Create data table based off of the given size.
      Table<dim,double> data_table;
      data_table.TableBase<dim,double>::reinit(size_idx);
      TableIndices<dim> idx;

      // Loop through the data table and fill it with the velocities from FastScape.

      // Indexes through z, y, and then x.
      for (unsigned int k=0; k<data_table.size()[2]; ++k)
        {
          idx[2] = k;

          for (unsigned int i=0; i<data_table.size()[1]; ++i)
            {
              idx[1] = i;

              for (unsigned int j=0; j<data_table.size()[0]; ++j)
                {
                  idx[0] = j;

                  // Convert back to m/s.
                  data_table(idx) = values[array_size*i+j] / year_in_seconds;
                }
            }
        }
      return data_table;
    }



    template <int dim>
    bool
    FastScapecc<dim>::
    needs_surface_stabilization () const
    {
      return true;
    }

    template <int dim>
    void FastScapecc<dim>::declare_parameters(ParameterHandler &prm)
    {
      prm.enter_subsection("Geometry model");
      {
        // Declare parameters for the Box geometry
        // if (prm.get("Model name") == "box")
        // {
        //     prm.enter_subsection("Box");
        //     {
        //         prm.declare_entry("X repetitions", "1", Patterns::Integer(1),
        //                           "Number of cells in the X direction.");
        //         prm.declare_entry("Y repetitions", "1", Patterns::Integer(1),
        //                           "Number of cells in the Y direction.");
        //         prm.declare_entry("Z repetitions", "1", Patterns::Integer(1),
        //                           "Number of cells in the Z direction.");
        //     }
        //     prm.leave_subsection();  // End of Box
        // }
        // Declare parameters for the Spherical shell geometry
        // else if (prm.get("Model name") == "spherical shell")
        {
          prm.enter_subsection("Spherical shell");
          {
            prm.declare_entry("Inner radius", "3481000", Patterns::Double(0),
                              "The inner radius of the spherical shell.");
            prm.declare_entry("Outer radius", "6336000", Patterns::Double(0),
                              "The outer radius of the spherical shell.");
            prm.declare_entry("Opening angle", "360", Patterns::Double(0, 360),
                              "The opening angle of the spherical shell in degrees.");
          }
          prm.leave_subsection();
        }
      }
      prm.leave_subsection();

      prm.enter_subsection ("Mesh deformation");
      {
        prm.enter_subsection ("Fastscapecc");
        {
          prm.declare_entry("Number of steps", "10",
                            Patterns::Integer(),
                            "Number of steps per ASPECT timestep");
          prm.declare_entry("Maximum timestep", "10e3",
                            Patterns::Double(0),
                            "Maximum timestep for FastScape. Units: $\\{yrs}$");
          prm.declare_entry("Additional fastscape refinement levels", "0",
                            Patterns::Integer(),
                            "How many levels above ASPECT FastScape should be refined.");
          prm.declare_entry ("Use center slice for 2d", "false",
                             Patterns::Bool (),
                             "If this is set to true, then a 2D model will only consider the "
                             "center slice FastScape gives. If set to false, then aspect will"
                             "average the mesh along Y excluding the ghost nodes.");
          prm.declare_entry("Fastscape seed", "1000",
                            Patterns::Integer(),
                            "Seed used for adding an initial noise to FastScape topography based on the initial noise magnitude.");
          prm.declare_entry("Maximum surface refinement level", "1",
                            Patterns::Integer(),
                            "This should be set to the highest ASPECT refinement level expected at the surface.");
          prm.declare_entry("Surface refinement difference", "0",
                            Patterns::Integer(),
                            "The difference between the lowest and highest refinement level at the surface. E.g., if three resolution "
                            "levels are expected, this would be set to 2.");
          prm.declare_entry("Y extent in 2d", "100000",
                            Patterns::Double(),
                            "FastScape Y extent when using a 2D ASPECT model. Units: $\\{m}$");
          prm.declare_entry ("Use velocities", "true",
                             Patterns::Bool (),
                             "Flag to use FastScape advection and uplift.");
          prm.declare_entry("Precision", "0.001",
                            Patterns::Double(),
                            "Precision value for how close a ASPECT node must be to the FastScape node for the value to be transferred.");
          prm.declare_entry("Initial noise magnitude", "5",
                            Patterns::Double(),
                            "Maximum topography change from the initial noise. Units: $\\{m}$");

          prm.enter_subsection ("Boundary conditions");
          {
            prm.declare_entry ("Front", "1",
                               Patterns::Integer (0, 1),
                               "Front (bottom) boundary condition, where 1 is fixed and 0 is reflective.");
            prm.declare_entry ("Right", "1",
                               Patterns::Integer (0, 1),
                               "Right boundary condition, where 1 is fixed and 0 is reflective.");
            prm.declare_entry ("Back", "1",
                               Patterns::Integer (0, 1),
                               "Back (top) boundary condition, where 1 is fixed and 0 is reflective.");
            prm.declare_entry ("Left", "1",
                               Patterns::Integer (0, 1),
                               "Left boundary condition, where 1 is fixed and 0 is reflective.");
          }
          prm.leave_subsection();

          prm.enter_subsection ("Erosional parameters");
          {
            prm.declare_entry("Drainage area exponent", "0.4",
                              Patterns::Double(),
                              "Exponent for drainage area.");
            prm.declare_entry("Slope exponent", "1",
                              Patterns::Double(),
                              "The  slope  exponent  for  SPL (n).  Generally  m/n  should  equal  approximately 0.4");
            prm.declare_entry("Bedrock river incision rate", "1e-5",
                              Patterns::Double(),
                              "River incision rate for bedrock in the Stream Power Law. Units: $\\{m^(1-2*drainage_area_exponent)/yr}$");
            prm.declare_entry("Sediment river incision rate", "-1",
                              Patterns::Double(),
                              "River incision rate for sediment in the Stream Power Law. -1 sets this to the bedrock river incision rate. Units: $\\{m^(1-2*drainage_area_exponent)/yr}$ ");
            prm.declare_entry("Bedrock diffusivity", "1e-2",
                              Patterns::Double(),
                              "Transport coefficient (diffusivity) for bedrock. Units: $\\{m^2/yr}$ ");
            prm.declare_entry("Sediment diffusivity", "-1",
                              Patterns::Double(),
                              "Transport coefficient (diffusivity) for sediment. -1 sets this to the bedrock diffusivity. Units: $\\{m^2/yr}$");
            prm.declare_entry("Elevation factor", "1",
                              Patterns::Double(),
                              "Amount to multiply kf and kd by past given orographic elevation control.");
          }
          prm.leave_subsection();
        }
        prm.leave_subsection();
      }
      prm.leave_subsection ();
    }


    template <int dim>
    void FastScapecc<dim>::parse_parameters(ParameterHandler &prm)
    {
      prm.enter_subsection("Geometry model");
      {
        if (prm.get("Model name") == "box")
          {
            geometry_type = GeometryType::Box;
          }
        else if (prm.get("Model name") == "spherical shell")
          {
            geometry_type = GeometryType::SphericalShell;
          }
      }
      prm.leave_subsection();


      end_time = prm.get_double ("End time");
      if (prm.get_bool ("Use years in output instead of seconds") == true)
        end_time *= year_in_seconds;

      // prm.enter_subsection("Geometry model");
      // {
      //   prm.enter_subsection("Box");
      //   {
      //     repetitions[0] = prm.get_integer ("X repetitions");
      //     if (dim >= 2)
      //       {
      //         repetitions[1] = prm.get_integer ("Y repetitions");
      //       }
      //     if (dim >= 3)
      //       {
      //         repetitions[dim-1] = prm.get_integer ("Z repetitions");
      //       }
      //   }
      //   prm.leave_subsection();
      // }
      // prm.leave_subsection();



      prm.enter_subsection("Geometry model");
      {
        // Parse parameters for the Box geometry
        if (prm.get("Model name") == "box")
          {
            prm.enter_subsection("Box");
            {
              repetitions[0] = prm.get_integer("X repetitions");
              repetitions[1] = prm.get_integer("Y repetitions");
              if (dim == 3)
                repetitions[2] = prm.get_integer("Z repetitions");
            }
            prm.leave_subsection();  // End of Box
          }
        // Parse parameters for the Spherical shell geometry
        else if (prm.get("Model name") == "spherical shell")
          {
            prm.enter_subsection("Spherical shell");
            {
              inner_radius = prm.get_double("Inner radius");
              outer_radius = prm.get_double("Outer radius");
              opening_angle = prm.get_double("Opening angle");
            }
            prm.leave_subsection();
          }
      }
      prm.leave_subsection();

      prm.enter_subsection ("Mesh deformation");
      {
        prm.enter_subsection("Fastscapecc");
        {
          fastscape_steps_per_aspect_step = prm.get_integer("Number of steps");
          maximum_fastscape_timestep = prm.get_double("Maximum timestep");
          additional_refinement_levels = prm.get_integer("Additional fastscape refinement levels");
          center_slice = prm.get_bool("Use center slice for 2d");
          fs_seed = prm.get_integer("Fastscape seed");
          maximum_surface_refinement_level = prm.get_integer("Maximum surface refinement level");
          surface_refinement_difference = prm.get_integer("Surface refinement difference");
          y_extent_2d = prm.get_double("Y extent in 2d");
          precision = prm.get_double("Precision");
          noise_h = prm.get_double("Initial noise magnitude");

          if (!this->convert_output_to_years())
            {
              maximum_fastscape_timestep /= year_in_seconds;
            }

          prm.enter_subsection("Boundary conditions");
          {
            bottom = prm.get_integer("Front");
            right = prm.get_integer("Right");
            top = prm.get_integer("Back");
            left = prm.get_integer("Left");
          }
          prm.leave_subsection();

          prm.enter_subsection("Erosional parameters");
          {
            m = prm.get_double("Drainage area exponent");
            n = prm.get_double("Slope exponent");
            kfsed = prm.get_double("Sediment river incision rate");
            kff = prm.get_double("Bedrock river incision rate");
            kdsed = prm.get_double("Sediment diffusivity");
            kdd = prm.get_double("Bedrock diffusivity");

            // if (!this->convert_output_to_years())
            //   {
            //     kff *= year_in_seconds;
            //     kdd *= year_in_seconds;
            //     kfsed *= year_in_seconds;
            //     kdsed *= year_in_seconds;
            //   }

          }
          prm.leave_subsection();

        }
        prm.leave_subsection();
      }
      prm.leave_subsection ();
    }
  }
}


// explicit instantiation of the functions we implement in this file
namespace aspect
{
  namespace MeshDeformation
  {
    ASPECT_REGISTER_MESH_DEFORMATION_MODEL(FastScapecc,
                                           "fastscapecc",
                                           "A plugin, which prescribes the surface mesh to "
                                           "deform according to an analytically prescribed "
                                           "function. Note that the function prescribes a "
                                           "deformation velocity, i.e. the return value of "
                                           "this plugin is later multiplied by the time step length "
                                           "to compute the displacement increment in this time step. "
                                           "The format of the "
                                           "functions follows the syntax understood by the "
                                           "muparser library, see Section~\\ref{sec:muparser-format}.")
  }
}
