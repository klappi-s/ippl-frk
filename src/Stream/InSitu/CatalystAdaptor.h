#ifndef CatalystAdaptor_h
#define CatalystAdaptor_h

#include "Ippl.h"

#include <catalyst.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <variant>
#include <utility>
#include <type_traits>
#include "Utility/IpplException.h"

#include<filesystem>

/* catalyst header defined the following for free use ... */
//   CATALYST_EXPORT enum catalyst_status catalyst_initialize(const conduit_node* params);
//   CATALYST_EXPORT enum catalyst_status catalyst_finalize(const conduit_node* params);
//   CATALYST_EXPORT enum catalyst_status catalyst_about(conduit_node* params);
//   CATALYST_EXPORT enum catalyst_status catalyst_results(conduit_node* params);


namespace CatalystAdaptor {


    using View_vector =
        Kokkos::View<ippl::Vector<double, 3>***, Kokkos::LayoutLeft, Kokkos::HostSpace>;
    inline void setData(conduit_cpp::Node& node, const View_vector& view) {
        node["electrostatic/association"].set_string("element");
        node["electrostatic/topology"].set_string("mesh");
        node["electrostatic/volume_dependent"].set_string("false");

        auto length = std::size(view);

        // offset is zero as we start without the ghost cells
        // stride is 1 as we have every index of the array
        node["electrostatic/values/x"].set_external(&view.data()[0][0], length, 0, 1);
        node["electrostatic/values/y"].set_external(&view.data()[0][1], length, 0, 1);
        node["electrostatic/values/z"].set_external(&view.data()[0][2], length, 0, 1);
    }


    using View_scalar = Kokkos::View<double***, Kokkos::LayoutLeft, Kokkos::HostSpace>;
    inline void setData(conduit_cpp::Node& node, const View_scalar& view) {
        node["density/association"].set_string("element");
        node["density/topology"].set_string("mesh");
        node["density/volume_dependent"].set_string("false");

        node["density/values"].set_external(view.data(), view.size());
    }



/*  sets a file path to a certain node, first tries to fetch from environment, afterwards uses the dafault path passed  */
    void set_node_script(
        conduit_cpp::Node node_path,
        const char* env_var,
        const std::filesystem::path default_file_path){
            
        const char* file_path_env = std::getenv(env_var);
        std::filesystem::path file_path;

        
       if (file_path_env && std::filesystem::exists(file_path_env)) {
           std::cout << "Using " << env_var << " from environment: " << file_path_env << std::endl;
           file_path = file_path_env;
       } else {
           std::cout << "No valid " << env_var <<" set. Using default: " << default_file_path << std::endl;
           file_path = default_file_path;
       }

       node_path.set(file_path.string());
    }



    void Initialize() {
        conduit_cpp::Node node;
        std::filesystem::path source_dir = std::filesystem::path(__FILE__).parent_path();
               
               
        // set_node_script( node["catalyst/proxies/proxy/filename"],
        //                 "CATALYST_PROXYS_PATH",
        //                 source_dir / "catalyst_scripts" / "proxy_default.xml"
        // );
        

        
        set_node_script( node["catalyst/proxies/proxy_e/filename"],
                        "CATALYST_PROXYS_PATH",
                        source_dir / "catalyst_scripts" / "proxy_default_electric.xml"
        );
        set_node_script( node["catalyst/proxies/proxy_m/filename"],
                        "CATALYST_PROXYS_PATH",
                        source_dir / "catalyst_scripts" / "proxy_default_magnetic.xml"
        );

        
        
        set_node_script( node["catalyst/scripts/script/filename"],
                        "CATALYST_PIPELINE_PATH",
                        source_dir / "catalyst_scripts" / "pipeline_default.py"
                    );

        set_node_script( node["catalyst/scripts/extract0/filename"],
                        "CATALYST_EXTRACTOR_SCRIPT_P",
                        source_dir /"catalyst_scripts" / "catalyst_extractors" /"png_ext_particle.py"
                    );

        set_node_script( node["catalyst/scripts/extract1/filename"],
                        "CATALYST_EXTRACTOR_SCRIPT_S",
                        source_dir /"catalyst_scripts" / "catalyst_extractors" /"png_ext_sfield.py"
                    );

        set_node_script( node["catalyst/scripts/extract2/filename"],
                        "CATALYST_EXTRACTOR_SCRIPT_V",
                        source_dir /"catalyst_scripts" / "catalyst_extractors" /"png_ext_vfield.py"
                    );




        std::cout << "ippl: catalyst_initialize() =>" << std::endl;
        catalyst_status err = catalyst_initialize(conduit_cpp::c_node(&node));
        if (err != catalyst_status_ok) {

            std::cout << "\n Catalyst initialized fail.....\n" << std::endl;
            throw IpplException("Stream::InSitu::CatalystAdaptor", "Failed to initialize Catalyst");
            // std::cerr << "Failed to initialize Catalyst: " << err << std::endl;
        }
        else{
            std::cout << "\n Catalyst initialized successfully.\n" << std::endl;
        }
    }


    void Finalize() {
        conduit_cpp::Node node;
        catalyst_status err = catalyst_finalize(conduit_cpp::c_node(&node));
        if (err != catalyst_status_ok) {
            std::cerr << "Failed to finalize Catalyst: " << err << std::endl;
        }
    }


    void Execute_Particle(
         const std::string& particlesName ,
         const auto& particleContainer,
         const auto& R_host, const auto& P_host, const auto& q_host, const auto& ID_host,
         conduit_cpp::Node& node) {

        // channel for particles
        auto channel = node["catalyst/channels/ippl_" + particlesName];
        channel["type"].set_string("mesh");

        // in data channel now we adhere to conduits mesh blueprint definition
        auto mesh = channel["data"];
        mesh["coordsets/coords/type"].set_string("explicit");

        //mesh["coordsets/coords/values/x"].set_external(&layout_view.data()[0][0], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        //mesh["coordsets/coords/values/y"].set_external(&layout_view.data()[0][1], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        //mesh["coordsets/coords/values/z"].set_external(&layout_view.data()[0][2], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        mesh["coordsets/coords/values/x"].set_external(&R_host.data()[0][0], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        mesh["coordsets/coords/values/y"].set_external(&R_host.data()[0][1], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        mesh["coordsets/coords/values/z"].set_external(&R_host.data()[0][2], particleContainer->getLocalNum(), 0, sizeof(double)*3);

        mesh["topologies/mesh/type"].set_string("unstructured");
        mesh["topologies/mesh/coordset"].set_string("coords");
        mesh["topologies/mesh/elements/shape"].set_string("point");
        //mesh["topologies/mesh/elements/connectivity"].set_external(particleContainer->ID.getView().data(),particleContainer->getLocalNum());
        mesh["topologies/mesh/elements/connectivity"].set_external(ID_host.data(),particleContainer->getLocalNum());

        //auto charge_view = particleContainer->getQ().getView();

        // add values for scalar charge field
        auto fields = mesh["fields"];
        fields["charge/association"].set_string("vertex");
        fields["charge/topology"].set_string("mesh");
        fields["charge/volume_dependent"].set_string("false");

        //fields["charge/values"].set_external(particleContainer->q.getView().data(), particleContainer->getLocalNum());
        fields["charge/values"].set_external(q_host.data(), particleContainer->getLocalNum());

        // add values for vector velocity field
        //auto velocity_view = particleContainer->P.getView();
        fields["velocity/association"].set_string("vertex");
        fields["velocity/topology"].set_string("mesh");
        fields["velocity/volume_dependent"].set_string("false");

        //fields["velocity/values/x"].set_external(&velocity_view.data()[0][0], particleContainer->getLocalNum(),0 ,sizeof(double)*3);
        //fields["velocity/values/y"].set_external(&velocity_view.data()[0][1], particleContainer->getLocalNum(),0 ,sizeof(double)*3);
        //fields["velocity/values/z"].set_external(&velocity_view.data()[0][2], particleContainer->getLocalNum(),0 ,sizeof(double)*3);
        fields["velocity/values/x"].set_external(&P_host.data()[0][0], particleContainer->getLocalNum(),0 ,sizeof(double)*3);
        fields["velocity/values/y"].set_external(&P_host.data()[0][1], particleContainer->getLocalNum(),0 ,sizeof(double)*3);
        fields["velocity/values/z"].set_external(&P_host.data()[0][2], particleContainer->getLocalNum(),0 ,sizeof(double)*3);

        fields["position/association"].set_string("vertex");
        fields["position/topology"].set_string("mesh");
        fields["position/volume_dependent"].set_string("false");

        //fields["position/values/x"].set_external(&layout_view.data()[0][0], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        //fields["position/values/y"].set_external(&layout_view.data()[0][1], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        //fields["position/values/z"].set_external(&layout_view.data()[0][2], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        fields["position/values/x"].set_external(&R_host.data()[0][0], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        fields["position/values/y"].set_external(&R_host.data()[0][1], particleContainer->getLocalNum(), 0, sizeof(double)*3);
        fields["position/values/z"].set_external(&R_host.data()[0][2], particleContainer->getLocalNum(), 0, sizeof(double)*3);
    }
    



    /* this needs to be overworked ... */
    template <class Field>  // == ippl::Field<double, 3, ippl::UniformCartesian<double, 3>, Cell>*
    void Execute_Field(const std::string& fieldName, Field* field, 
         Kokkos::View<typename Field::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace>& host_view_layout_left,
         conduit_cpp::Node& node) {
        static_assert(Field::dim == 3, "CatalystAdaptor only supports 3D");

        // A) define mesh

        // add catalyst channel named ippl_"field", as fields is reserved
        auto channel = node["catalyst/channels/ippl_" + fieldName];
        channel["type"].set_string("mesh");

        // in data channel now we adhere to conduits mesh blueprint definition
        auto mesh = channel["data"];
        mesh["coordsets/coords/type"].set_string("uniform");

        // number of points in specific dimension
        std::string field_node_dim{"coordsets/coords/dims/i"};
        std::string field_node_origin{"coordsets/coords/origin/x"};
        std::string field_node_spacing{"coordsets/coords/spacing/dx"};

        for (unsigned int iDim = 0; iDim < field->get_mesh().getGridsize().dim; ++iDim) {
            // add dimension
            mesh[field_node_dim].set(field->getLayout().getLocalNDIndex()[iDim].length() + 1);

            // add origin
            mesh[field_node_origin].set(
                field->get_mesh().getOrigin()[iDim] + field->getLayout().getLocalNDIndex()[iDim].first()
                      * field->get_mesh().getMeshSpacing(iDim));

            // add spacing
            mesh[field_node_spacing].set(field->get_mesh().getMeshSpacing(iDim));

            // increment last char in string
            ++field_node_dim.back();
            ++field_node_origin.back();
            ++field_node_spacing.back();
        }

        // add topology
        mesh["topologies/mesh/type"].set_string("uniform");
        mesh["topologies/mesh/coordset"].set_string("coords");
        std::string field_node_origin_topo{"topologies/mesh/origin/x"};
        for (unsigned int iDim = 0; iDim < field->get_mesh().getGridsize().dim; ++iDim) {
            // shift origin
            mesh[field_node_origin_topo].set(field->get_mesh().getOrigin()[iDim]
                                             + field->getLayout().getLocalNDIndex()[iDim].first()
                                                   * field->get_mesh().getMeshSpacing(iDim));

            // increment last char in string ('x' becomes 'y' becomes 'z')
            ++field_node_origin_topo.back();
        }

        // B) Set the field values

        // Initialize the existing Kokkos::View
        host_view_layout_left = Kokkos::View<typename Field::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace>(
           "host_view_layout_left",
           field->getLayout().getLocalNDIndex()[0].length(),
           field->getLayout().getLocalNDIndex()[1].length(),
           field->getLayout().getLocalNDIndex()[2].length());

        // Creates a host-accessible mirror view and copies the data from the device view to the host.
        auto host_view =
            Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), field->getView());

        // Copy data from field to the memory+style which will be passed to Catalyst
        auto nGhost = field->getNghost();
        for (size_t i = 0; i < field->getLayout().getLocalNDIndex()[0].length(); ++i) {
            for (size_t j = 0; j < field->getLayout().getLocalNDIndex()[1].length(); ++j) {
                for (size_t k = 0; k < field->getLayout().getLocalNDIndex()[2].length(); ++k) {
                    host_view_layout_left(i, j, k) = host_view(i + nGhost, j + nGhost, k + nGhost);
                }
            }
        }

        // Add values and subscribe to data
        auto fields = mesh["fields"];
        setData(fields, host_view_layout_left);
    }



        // Handle fields
        // // Map of all Kokkos::Views. This keeps a reference on all Kokkos::Views
        // // which ensures that Kokkos does not free the memory before the end of this function.


        /* SCALAR FIELDS - handles both reference and shared_ptr */
        // == ippl::Field<double, 3, ippl::UniformCartesian<double, 3>, Cell>*
    template<typename T, unsigned Dim, class... ViewArgs>
    void execute_entry(const ippl::Field<T, Dim, ViewArgs...>& entry, const std::string label, conduit_cpp::Node& node, ViewRegistry& vr) {
        std::cout << "      execute_entry(ippl::Field<" << typeid(T).name() << "," << Dim << ">) called" << std::endl;
        
        
        // std::map<std::string, Kokkos::View<typename Field_t<Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> > scalar_host_views;
        
        Kokkos::View<typename Field_t<Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> scalar_host_view;
        
        const Field_t<Dim>* field = &entry;
        /* creates amp entry with default initialized values ... */
        // Execute_Field(label, field, scalar_host_views[label], node);
        Execute_Field(label, field, scalar_host_view, node);

        vr.set(scalar_host_view);
    }






        /* VECTOR FIELDS - handles both reference and shared_ptr */
        // == ippl::Field<ippl::Vector<double, 3>, 3, ippl::UniformCartesian<double, 3>, Cell>*
    template<typename T, unsigned Dim, unsigned Dim_v, class... ViewArgs>
    void execute_entry(const ippl::Field<ippl::Vector<T, Dim_v>, Dim, ViewArgs...>& entry, const std::string label,conduit_cpp::Node& node, ViewRegistry& vr) {
        std::cout << "      execute_entry(ippl::Field<ippl::Vector<" << typeid(T).name() << "," << Dim_v << ">," << Dim << ">) called" << std::endl;
        
        const VField_t<T, Dim>* field = &entry;
        
        // std::map<std::string, Kokkos::View<typename VField_t<T, Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> > vector_host_views;
        // Execute_Field(label,field, vector_host_views[label], node); 


        /* use make shaed so we dont pass adereference to a nullptr .... */
        /* for us  more conveneient approach */
        // std::shared<Kokkos::View<typename VField_t<T, Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> > vector_host_views =
        // std::make_shared<Kokkos::View<typename VField_t<T, Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> >;

        Kokkos::View<typename VField_t<T, Dim>::view_type::data_type, Kokkos::LayoutLeft, Kokkos::HostSpace> vector_host_view;

        
        Execute_Field(label,field, vector_host_view, node); 



        
        vr.set(vector_host_view);
        /* save a copy to shaed pointer, keeps dynamco reference to the View alive
        saving a reference would make sense since referenced object will not exist when exiting this scope ... */
        
    }


        // const std::string& fieldName = "E";
    // You remove const for particles in your execute_entry function
    // because you need to call non-const member functions 
    // (like getHostMirror()) on the particle attributes, which
    // are not marked as const in their class definition. For 
    // fields, you can keep const because the field-related
    // functions you call (like getView(), getLayout(), etc.) 
    // are either const or do not modify the object.




    /* instead of maps storing kokkos view in scope we use the registry to keep everything in frame .... and be totally type indepedent
    we can set with name (but since we likely will not have the need to ever retrieve we can just stire nameless
    to redzcede unncessary computin type ...) */


    // PARTICLECONTAINERS DERIVED FROM PARTICLEBASE:
    template<typename T>
    requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
    void execute_entry(const T& entry, const std::string particlesName,  conduit_cpp::Node& node, ViewRegistry& vr) {
        std::cout   << "      execute_entry(ParticleBase<PLayout<" 
                    << typeid(particle_value_t<T>).name() 
                    << ","
                    << particle_dim_v<T> 
                    << ",...>...> [or subclass]) called" << std::endl;


            ippl::ParticleAttrib<ippl::Vector<double, 3>>::HostMirror    R_host_view;
            ippl::ParticleAttrib<ippl::Vector<double, 3>>::HostMirror    P_host_view;
            ippl::ParticleAttrib<double>::HostMirror                     q_host_view;
            ippl::ParticleAttrib<std::int64_t>::HostMirror              ID_host_view;

            auto particleContainer = &entry;
            assert((particleContainer->ID.getView().data() != nullptr) && "ID view should not be nullptr, might be missing the right execution space");

            R_host_view  = particleContainer->R.getHostMirror();
            P_host_view  = particleContainer->P.getHostMirror();
            q_host_view  = particleContainer->q.getHostMirror();
            ID_host_view = particleContainer->ID.getHostMirror();

            Kokkos::deep_copy(R_host_view ,  particleContainer->R.getView());
            Kokkos::deep_copy(P_host_view ,  particleContainer->P.getView());
            Kokkos::deep_copy(q_host_view ,  particleContainer->q.getView());
            Kokkos::deep_copy(ID_host_view, particleContainer->ID.getView());





            Execute_Particle(
                  particlesName,
                  particleContainer,
                    R_host_view,
                    P_host_view,
                    q_host_view,
                    ID_host_view,
                  node
            );



            vr.set(R_host_view);
            vr.set(P_host_view);
            vr.set(q_host_view);
            vr.set(ID_host_view);
    }


    // BASE CASE: only enabled if EntryT is NOT derived from ippl::ParticleBaseBase
    template<typename T>
    requires (!std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>)
    void execute_entry(const std::string label, [[maybe_unused]] T&& entry, conduit_cpp::Node& node, ViewRegistry& vr) {
        std::cout << "  Entry type can't be processed: ID "<< label <<" "<< typeid(std::decay_t<T>).name() << std::endl;
    }


    /* SHARED_PTR DISPATCHER - automatically unwraps and dispatches to appropriate overload */
    template<typename T>
    void execute_entry( const std::shared_ptr<T>& entry,const std::string  label, conduit_cpp::Node& node, ViewRegistry& vr ) {
        if (entry) {
            // std::cout << "  dereferencing shared pointer and reattempting execute..." << std::endl;
            execute_entry(*entry, label,  node, vr);  // Dereference and dispatch to reference version
        } else {
            std::cout << "  Null shared_ptr encountered" << std::endl;
        }
    }





    template<typename T>
    void AddSteerableChannel( T steerable_scalar_forwardpass, std::string steerable_suffix, conduit_cpp::Node& node) {
        std::cout << "AddSteerableChanelValue( " << steerable_suffix << "); | Type: " << typeid(T).name() << std::endl;
        
        
        auto steerable_channel = node["catalyst/channels/steerable_channel_forward_" + steerable_suffix];

        steerable_channel["type"].set("mesh");
        auto steerable_data = steerable_channel["data"];
        steerable_data["coordsets/coords/type"].set_string("explicit");
        steerable_data["coordsets/coords/values/x"].set_float64_vector({ 1 });
        steerable_data["coordsets/coords/values/y"].set_float64_vector({ 2 });
        steerable_data["coordsets/coords/values/z"].set_float64_vector({ 3 });
        steerable_data["topologies/mesh/type"].set("unstructured");
        steerable_data["topologies/mesh/coordset"].set("coords");
        steerable_data["topologies/mesh/elements/shape"].set("point");
        steerable_data["topologies/mesh/elements/connectivity"].set_int32_vector({ 0 });


        conduit_cpp::Node steerable_field = steerable_data["fields/steerable_field_f_" + steerable_suffix];
        steerable_field["association"].set("vertex");
        steerable_field["topology"].set("mesh");
        steerable_field["volume_dependent"].set("false");

        conduit_cpp::Node values = steerable_field["values"];


        if constexpr (std::is_same_v<T, double>) {
            values.set_float64_vector({steerable_scalar_forwardpass});
        } else if constexpr (std::is_same_v<T, float>) {
            values.set_float32_vector({steerable_scalar_forwardpass});
        } else if constexpr (std::is_same_v<T, int>) {
            values.set_int64_vector({steerable_scalar_forwardpass});
        } else if constexpr (std::is_same_v<T, unsigned int>) {
            values.set_uint64_vector({steerable_scalar_forwardpass});
        } else {
            throw IpplException("CatalystAdaptor::AddSteerableChannel", "Unsupported steerable type for channel: " + steerable_suffix);
        }
        
    }


    /* maybe use function overloading instead ... */
        // const std::string value_path = ... 
        // steerable_scalar_backwardpass = static_cast<T>(results[value_path].value());
        // steerable_scalar_backwardpass = results[value_path].value()[0];
        /* ????? this should work?? */
        // if constexpr (std::is_same_v<std::remove_cvref_t<T>, double>) {

    template<typename T>
    void FetchSteerableChannelValue( T& steerable_scalar_backwardpass, std::string steerable_suffix, conduit_cpp::Node& results) {
        std::cout << "FetchSteerableChanelValue(" << steerable_suffix  << ") | Type: " << typeid(T).name() << std::endl;

            
            conduit_cpp::Node steerable_channel     = results["catalyst/steerable_channel_backward_" + steerable_suffix];
            conduit_cpp::Node steerable_field       = steerable_channel[ "fields/steerable_field_b_" + steerable_suffix];
            conduit_cpp::Node values = steerable_field["values"];

        if constexpr (std::is_same_v<T, double>) {
            if (steerable_field["values"].dtype().is_number()) {
                steerable_scalar_backwardpass = steerable_field["values"].to_double();
                // std::cout << "value scalar fetched" << std::endl;
            }
            // else if (steerable_field["values"].dtype().is_float64()) {
            //     auto ptr = steerable_field["values"].as_float64_ptr();
            //     if (ptr){
            //         steerable_scalar_backwardpass = ptr[0];
            //         std::cout << "value vector fetched ..." << std::endl;
            //     }
            //     else throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Null pointer for steerable value: " + steerable_suffix);
            // }
            else {
                throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Unsupported steerable value type for channel: " + steerable_suffix);
            }
        }
         else if constexpr (std::is_same_v<T, float>) {
            if (steerable_field["values"].dtype().is_number()) {
                steerable_scalar_backwardpass = steerable_field["values"].to_double();
            }
            // else if (steerable_field["values"].dtype().is_float32()) {
            //     auto ptr = steerable_field["values"].as_float64_ptr();
            //     if (ptr) steerable_scalar_backwardpass = ptr[0];
            //     else throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Null pointer for steerable value: " + steerable_suffix);
            // }
            else {
                throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Unsupported steerable value type for channel: " + steerable_suffix);
            }
        }
        else if constexpr (std::is_same_v<T, int>) {
            if (steerable_field["values"].dtype().is_number()) {
                steerable_scalar_backwardpass = steerable_field["values"].to_int32();
            }
            // else if (steerable_field["values"].dtype().is_float64()) {
            //     auto ptr = steerable_field["values"].as_float64_ptr();
            //     if (ptr) steerable_scalar_backwardpass = ptr[0];
            //     else throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Null pointer for steerable value: " + steerable_suffix);
            // }
            else {
                throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Unsupported steerable value type for channel: " + steerable_suffix);
            }
        }
        else if constexpr (std::is_same_v<T, unsigned int>) {
            if (steerable_field["values"].dtype().is_number()) {
                steerable_scalar_backwardpass = steerable_field["values"].to_uint32();
            }
            // else if (steerable_field["values"].dtype().is_float64()) {
            //     auto ptr = steerable_field["values"].as_float64_ptr();
            //     if (ptr) steerable_scalar_backwardpass = ptr[0];
            //     else throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Null pointer for steerable value: " + steerable_suffix);
            // }
            else {
                throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Unsupported steerable value type for channel: " + steerable_suffix);
            }
        }
        
        else {
            std::cout << "failed to fetch value" << std::endl;
            throw IpplException("CatalystAdaptor::FetchSteerableChannelValue", "Unsupported steerable type for channel: " + steerable_suffix);
        } 
    }
        



    void Results(conduit_cpp::Node& results) {
        
        // conduit_cpp::Node results;
        catalyst_status err = catalyst_results(conduit_cpp::c_node(&results));
        // catalyst_status err = catalyst_results(conduit_cpp::c_node(&results));
        if (err != catalyst_status_ok)
        {
            std::cerr << "Failed to execute Catalyst-results: " << err << std::endl;
        }
        // else
        // {
        //     std::cout << "Result Node dump:" << std::endl;
        //     results.print();
        // }   
    }



    void Execute(
            auto& registry_vis, auto& registry_steer,
            int cycle, double time, int rank
        ){
        
        // add time/cycle information
        conduit_cpp::Node node;
        auto state = node["catalyst/state"];
        state["cycle"].set(cycle);
        state["time"].set(time);
        state["domain_id"].set(rank);     

        /* catch view registry by referrence and pass it to execute by refernece 
        viewregistry with shared pointer will be delted by registry running out of scope,
         shared pointers being deleted and deallocating the allocated copies for memories ...*/
        ViewRegistry vr;
        
        registry_steer.forEach(
            [&node](std::string_view label, const auto& entry) {
                // std::cout << "   Entry ID: " << label << "\n";
                AddSteerableChannel(entry, std::string(label), node);
            }
        );

        registry_vis.forEach(
            [&node, &vr](std::string_view label, const auto& entry){
                // std::cout << "  Entry ID: " << label << "\n";
                execute_entry(entry, std::string(label),  node, vr);
            }
        );
        /* std::cout << dump outgoing node << std::endl; */
        // node.print();
  
        // Pass Conduit node to Catalyst and execute extraction and visualisation
        catalyst_status err = catalyst_execute(conduit_cpp::c_node(&node));
        if (err != catalyst_status_ok) {
            std::cerr << "Failed to execute Catalyst: " << err << std::endl;
        }
        /* Catch steerables in results node */
        conduit_cpp::Node results;
        Results(results);  
        
        // /* transfer steearble scalars back to original locaton via registry*/
        registry_steer.forEach(
            [&results](std::string_view label, auto& entry) {
                // std::cout << "   Entry ID: " << label << "\n";
                FetchSteerableChannelValue(entry, std::string(label), results);
            }
        );
    }

}  // namespace CatalystAdaptor

#endif


// # protocol: initialize
// # Currently, the initialize protocol defines how to load the catalyst implementation library and how to pass scripts to load for analysis.

// # catalyst_load/*: (optional) Catalyst will attempt to use the metadata under this node to find the implementation name and location. If it is missing the environmental variables CATALYST_IMPLEMENTATION_NAME and CATALYST_IMPLEMENTATION_PATH will be queried. See the catalyst_initialize documentation of the Catalyst API.

// # catalyst/scripts: (optional) if present must either be a list or object node with child nodes that provides paths to the Python scripts to load for in situ analysis.

// # catalyst/scripts/[name]: (optional) if present can be a string or object. If it is a string it is interpreted as path to the Python script. If it is an object it can have the following attributes.

// # catalyst/scripts/[name]/filename: path to the Python script

// # catalyst/scripts/[name]/args: (optional) if present must be of type list with each child node of type string. To retrieve these arguments from the script itself use the get_args() method of the paraview catalyst module.








































    // void Initialize_Adios(int argc, char* argv[])
    // {
    //     conduit_cpp::Node node;
    //     for (int cc = 1; cc < argc; ++cc)
    //     {
    //         if (strstr(argv[cc], "xml"))
    //         {
    //             node["adios/config_filepath"].set_string(argv[cc]);
    //         }
    //         else
    //         {
    //             node["catalyst/scripts/script" +std::to_string(cc - 1)].set_string(argv[cc]);
    //         }
    //     }
    //     node["catalyst_load/implementation"] = getenv("CATALYST_IMPLEMENTATION_NAME");
    //     catalyst_status err = catalyst_initialize(conduit_cpp::c_node(&node));
    //     if (err != catalyst_status_ok)
    //     {
    //         std::cerr << "Failed to initialize Catalyst: " << err << std::endl;
    //     }
    // }







    
            // catalyst blueprint definition
            // https://docs.paraview.org/en/latest/Catalyst/blueprints.html
            //
            // conduit blueprint definition (v.8.3)
            // https://llnl-conduit.readthedocs.io/en/latest/blueprint_mesh.html




/* Doesn't work ... */
/* SFINAEEEEE */

// // Detection idiom for addAttribute
// template<typename, typename = void>
// struct has_addAttribute : std::false_type {};

// template<typename T>
// struct has_addAttribute<T, std::void_t<decltype(&T::addAttribute)>> : std::true_type {};

// std::enable_if_t<!has_addAttribute<EntryT>::value,void>  execute_entry([[maybe_unused]] EntryT&& entry) {
// or
// template<typename EntryT, std::enable_if_t<!has_addAttribute<EntryT>::value, int> = 0>
// void execute_entry([[maybe_unused]] EntryT&& entry) {
    //    std::cout << "AA  Entry type can't be processed: " << typeid(std::decay_t<EntryT>).name() << std::endl;   


            
//     // using Base = ippl::ParticleBase<typename T::Layout_t>;
//     using Layout = typename T::Layout_t;
//     using value_type = typename Layout::vector_type::value_type;





        // using vector_type            = typename PLayout::vector_type;
        // using index_type             = typename PLayout::index_type;
        // using particle_position_type = typename PLayout::particle_position_type;
        // using particle_index_type    = ParticleAttrib<index_type, IDProperties...>;

        // using Layout_t = PLayout;

        // template <typename... Properties>
        // using attribute_type = typename detail::ParticleAttribBase<Properties...>;

        // template <typename MemorySpace>
        // using container_type = std::vector<attribute_type<MemorySpace>*>;

        // using attribute_container_type =
        //     typename detail::ContainerForAllSpaces<container_type>::type;

        // using bc_container_type = typename PLayout::bc_container_type;

        // using hash_container_type = typename detail::ContainerForAllSpaces<detail::hash_type>::type;

        // using size_type = detail::size_type;

        // std::cout << "All declared IDs:\n";
        // auto all_ids = registry_vis.getAllIds();
        // std::cout << "   ";
        // for (const auto& id : all_ids) {
        //     std::cout << "\"" << id << "\" ";
        // }
        // std::cout << "\n\n";
