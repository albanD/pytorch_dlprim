#include "CLTensor.h"
#include "utils.h"

#include <dlprim/core/util.hpp>
#include <dlprim/core/pointwise.hpp>

#include <iostream>
namespace ptdlprim {

using namespace torch;
using torch::autograd::tensor_list;
using torch::autograd::AutogradContext;


using c10::Device;
using c10::DeviceType;


    using torch::Tensor;
    
    torch::Tensor allocate_empty(torch::IntArrayRef size, c10::optional<ScalarType> dtype, c10::optional<Layout> /*layout*/, c10::optional<Device> device, c10::optional<bool> /*pin_memory*/, c10::optional<MemoryFormat> /*memory_format*/)
    {
        GUARD;
        c10::ScalarType st = dtype ? *dtype : c10::kFloat; 
        c10::Device dev = device ? *device : Device(OpenCLDeviceType,0);
        return ptdlprim::new_ocl_tensor(size,dev,st);
    }

    /// "aten::empty_strided"
    Tensor empty_strided(torch::IntArrayRef size, torch::IntArrayRef /*stride*/, c10::optional<ScalarType> dtype, c10::optional<Layout> layout, c10::optional<Device> device, c10::optional<bool> pin_memory) 
    {
        GUARD;
        return allocate_empty(size,dtype,layout,device,pin_memory,c10::nullopt);
    }

    torch::Tensor _reshape_alias(const Tensor & self, c10::IntArrayRef size, c10::IntArrayRef stride)
    {
        GUARD;
        torch::Tensor data = at::alias(self);
        data.getIntrusivePtr()->set_sizes_and_strides(size,stride);
        return data;
    }

    Tensor view(const Tensor & self, IntArrayRef size)
    {
        GUARD;
        torch::Tensor data=at::alias(self);
        TORCH_CHECK(data.is_contiguous(),"View imlemented on contiguous array");
        std::vector<int64_t> v(size.begin(),size.end());
        int64_t total=1,index=-1;
        for(unsigned i=0;i<v.size();i++) {
            if(v[i] == -1) {
                TORCH_CHECK(index==-1,"Must be unique -1");
                index=i;
            }
            else {
                total *= v[i];
            }
        }
        if(index != -1) {
            TORCH_CHECK(self.numel() % total == 0);
            v[index] = self.numel() / total;
        }
        else {
            TORCH_CHECK(total == self.numel());
        }
        c10::IntArrayRef new_size(v.data(),v.size());
        data.getIntrusivePtr()->set_sizes_contiguous(new_size);
        return data;
    }

    static Tensor make_contiguous_as_target_type(Tensor const &self,Tensor const &dst)
    {
        GUARD;
        Tensor c_src = self;
        if(self.dtype() != dst.dtype() || !self.is_contiguous()) {
            TensorOptions options = TensorOptions().dtype(dst.dtype()).memory_format(MemoryFormat::Contiguous);
            Tensor temp = at::empty_like(c_src,options);
            temp.copy_(c_src);
            c_src = temp;
        }
        return c_src;
    }

    Tensor _copy_from(const Tensor & self, const Tensor & dst, bool /*non_blocking*/)
    {
        GUARD;

        if(dst.device().type() == c10::DeviceType::CPU && self.device().type() == OpenCLDeviceType) {
            Tensor c_src = make_contiguous_as_target_type(self,dst);

            dlprim::Tensor t(todp(c_src));
            auto ec = getExecutionContext(self);
            if(dst.is_contiguous()) {
                void *ptr = dst.data_ptr();
                t.to_host(ec,ptr);
            }
            else {
                TensorOptions options = TensorOptions().memory_format(MemoryFormat::Contiguous);
                Tensor dst_c = at::empty_like(dst,options);
                void *ptr = dst_c.data_ptr();
                t.to_host(ec,ptr);
                dst.copy_(dst_c);
            }
        }
        else if(self.device().type() == c10::DeviceType::CPU && dst.device().type() == OpenCLDeviceType) {
            Tensor c_src = make_contiguous_as_target_type(self,dst);
            auto ec = getExecutionContext(dst);
            if(dst.is_contiguous()) {
                dlprim::Tensor t(todp(dst));
                t.to_device(ec,c_src.data_ptr());
            }
            else {
                TensorOptions options = TensorOptions().memory_format(MemoryFormat::Contiguous);
                Tensor temp = at::empty_like(dst,options);
                dlprim::Tensor t(todp(temp));
                t.to_device(ec,c_src.data_ptr());
                dst.copy_(temp);
            }
        }
        else if(self.device().type() == OpenCLDeviceType && dst.device().type() == OpenCLDeviceType) {
            if(self.is_contiguous() && dst.is_contiguous()) {
                dlprim::core::pointwise_operation_broadcast({todp(self)},{todp(dst)},{},"y0=x0;",getExecutionContext(self.device()));
            }
            else {
                auto src_sizes  = self.sizes();
                auto src_stride = self.strides();
                auto src_offset = self.storage_offset();
                auto tgt_sizes  = dst.sizes();
                auto tgt_stride = dst.strides();
                auto tgt_offset = dst.storage_offset();
                TORCH_CHECK(src_sizes == tgt_sizes);
                dlprim::Shape shape=dlprim::Shape::from_range(src_sizes.begin(),src_sizes.end());
                dlprim::Shape src_std=dlprim::Shape::from_range(src_stride.begin(),src_stride.end());
                dlprim::Shape tgt_std=dlprim::Shape::from_range(tgt_stride.begin(),tgt_stride.end());
                dlprim::core::copy_strided(shape,buffer_from_tensor(self),src_offset,src_std,
                                                 buffer_from_tensor(dst), tgt_offset,tgt_std,
                                                 todp(self.dtype()),
                                                 todp(dst.dtype()),
                                                 getExecutionContext(self.device()));
            }
            sync_if_needed(self.device());
        }
        else {
            throw std::runtime_error("OpenCL supports copy to CPU backend only");
        }
        return self;
    }

    Tensor &fill_(Tensor &self, const c10::Scalar &value)
    {
        GUARD;
        dlprim::Tensor t(todp(self));
        auto q = getExecutionContext(self);
        dlprim::core::fill_tensor(t,value.to<double>(),q);
        sync_if_needed(self.device());
        return self;
    }
    
    Tensor &zero_(Tensor &self)
    {
        GUARD;
        dlprim::Tensor t(todp(self));
        dlprim::core::fill_tensor(t,0.0,getExecutionContext(self));
        return self;
    }

    Tensor as_strided(const Tensor & self, IntArrayRef size, IntArrayRef stride, c10::optional<int64_t> storage_offset)
    {
        GUARD;
        Tensor result = at::alias(self);
        result.getIntrusivePtr()->set_sizes_and_strides(size,stride);
        if(storage_offset)
            result.getIntrusivePtr()->set_storage_offset(*storage_offset);
        return result;

    }

    // {"schema": "aten::_local_scalar_dense(Tensor self) -> Scalar", "dispatch": "True", "default": "False"}
    Scalar _local_scalar_dense(const Tensor & self)
    {
        GUARD;
        TORCH_CHECK(self.numel()==1);
        dlprim::Tensor x=todp(self);
        union {
            float f;
            double d;
            int8_t i8;
            uint8_t u8;
            int16_t i16;
            uint16_t u16;
            int32_t i32;
            uint32_t u32;
            int64_t i64;
            uint64_t u64;
            char data[16];
        } data;
        x.to_host(getExecutionContext(self),data.data);
        switch(x.dtype()) {
        case dlprim::float_data:    return data.f;
        case dlprim::double_data:   return data.d;
        case dlprim::int8_data:     return data.i8;
        case dlprim::uint8_data:    return data.u8;
        case dlprim::int16_data:    return data.i16;
        case dlprim::uint16_data:   return data.u16;
        case dlprim::int32_data:    return (int64_t)data.i32;
        case dlprim::uint32_data:   return (int64_t)data.u32;
        case dlprim::int64_data:    return (int64_t)data.i64;
        case dlprim::uint64_data:   return (int64_t)data.u64;
        default:
            TORCH_CHECK(!"Not implemented dtype","Not implemented data type");
        }
    }

    template<typename E,typename M>
    size_t select_impl_by(E *p,M *m,size_t n)
    {
        size_t N = 0;
        for(size_t i=0;i<n;i++) {
            if(m[i]) {
                p[N] = p[i];
                N++;
            }
        }
        return N;
    }

    template<typename T>
    size_t select_impl(T *mask,dlprim::Tensor &/*m*/,dlprim::Tensor &v)
    {
        void *p=v.host_data();
        switch(dlprim::size_of_data_type(v.dtype())) {
        case 1: return select_impl_by(static_cast<int8_t  *>(p),mask,v.shape().total_size());
        case 2: return select_impl_by(static_cast<int16_t *>(p),mask,v.shape().total_size());
        case 4: return select_impl_by(static_cast<int32_t *>(p),mask,v.shape().total_size());
        case 8: return select_impl_by(static_cast<int64_t *>(p),mask,v.shape().total_size());
        default:
            TORCH_CHECK(!"Invalid sizeof");
            return 0;
        }
    }
    
    // {"schema": "aten::masked_select(Tensor self, Tensor mask) -> Tensor", "dispatch": "True", "default": "False"}
    Tensor masked_select(const Tensor & self, const Tensor & mask)
    {
        GUARD;
        Tensor self_c = self.contiguous();
        Tensor mask_c = mask.contiguous();
        dlprim::Tensor x = todp(self_c);
        dlprim::Tensor m = todp(mask_c);
        TORCH_CHECK(x.shape() == m.shape(),"Broadasting is not implemented in masked_select yet");
        auto ec = getExecutionContext(self);
        x.to_host(ec);
        m.to_host(ec);
        size_t N = 0;
        switch(m.dtype()) {
        case dlprim::float_data:
            N = select_impl(m.data<float>(),m,x);
            break;
        case dlprim::double_data:
            N = select_impl(m.data<double>(),m,x);
            break;
        case dlprim::int8_data:
            N = select_impl(m.data<int8_t>(),m,x);
            break;
        case dlprim::uint8_data:
            N = select_impl(m.data<uint8_t>(),m,x);
            break;
        case dlprim::int16_data:
            N = select_impl(m.data<int16_t>(),m,x);
            break;
        case dlprim::uint16_data:
            N = select_impl(m.data<uint16_t>(),m,x);
            break;
        case dlprim::int32_data:
            N = select_impl(m.data<int32_t>(),m,x);
            break;
        case dlprim::uint32_data:
            N = select_impl(m.data<uint32_t>(),m,x);
            break;
        case dlprim::int64_data:
            N = select_impl(m.data<int64_t>(),m,x);
            break;
        case dlprim::uint64_data:
            N = select_impl(m.data<uint64_t>(),m,x);
            break;
        default:
            TORCH_CHECK(!"Not implemented dtype","Not implemented");
        }
        Tensor res=new_tensor_as(dlprim::Shape(N),self);
        if(N > 0) {
            dlprim::Tensor y=todp(res);
            y.to_device(getExecutionContext(self),x.host_data());
        }
        sync_if_needed(self.device());
        return res;
    }



} // namespace dtype

TORCH_LIBRARY_IMPL(aten, PrivateUse1, m) {
      m.impl("aten::empty.memory_format", &ptdlprim::allocate_empty);
      m.impl("aten::empty_strided",&ptdlprim::empty_strided);
      m.impl("aten::_reshape_alias",&ptdlprim::_reshape_alias);
      m.impl("aten::view",&ptdlprim::view);
      m.impl("aten::_copy_from",&ptdlprim::_copy_from);
      m.impl("aten::fill_.Scalar",&ptdlprim::fill_);
      m.impl("aten::zero_",&ptdlprim::zero_);
      m.impl("aten::as_strided",&ptdlprim::as_strided);
      m.impl("aten::_local_scalar_dense",&ptdlprim::_local_scalar_dense);
      m.impl("aten::masked_select",&ptdlprim::masked_select);
}
