#ifndef EXPU_CONTAINERS_DARRAY_HPP_INCLUDED
#define EXPU_CONTAINERS_DARRAY_HPP_INCLUDED

#include <iterator>
#include <memory>
#include <stdexcept>

#include "expu/containers/contiguous_container.hpp"

#include "expu/debug.hpp"
#include "expu/meta/meta_utils.hpp"
#include "expu/mem_utils.hpp"

namespace expu {

    template<
        class PtrType,
        class ConstPtrType>
    struct _darray_data {
    public:
        using pointer         = PtrType;
        using const_pointer   = ConstPtrType;

    public:
        PtrType first; //Starting address of container's allocated (if not nullptr) memory.
        PtrType last;  //Address of last element (Type) stored by container.
        PtrType end;   //One past end address of last allocated element of container.

        constexpr void steal(_darray_data&& other) noexcept
        {
            first = std::exchange(other.first, nullptr);
            last  = std::exchange(other.last , nullptr);
            end   = std::exchange(other.end  , nullptr);
        }
    };

    template<
        class Type,
        class Alloc = std::allocator<Type>>
    class darray
    {
    private:
        using _alloc_traits = std::allocator_traits<Alloc>;

        //Ensure allocator value_type matches the container type
        static_assert(std::is_same_v<Type, typename _alloc_traits::value_type>);

    //Essential typedefs (Container requirements)
    public:
        using allocator_type  = Alloc;
        using value_type      = Type;
        using reference       = Type&;
        using const_reference = const Type&;
        using pointer         = typename _alloc_traits::pointer;
        using const_pointer   = typename _alloc_traits::const_pointer;
        using difference_type = typename _alloc_traits::difference_type;
        using size_type       = typename _alloc_traits::size_type;

    private:
        using _data_t = _darray_data<pointer, const_pointer>;

    //Iterator typedefs
    public:
        using iterator       = ctg_iterator<_data_t>;
        using const_iterator = ctg_const_iterator<_data_t>;

    //Special constructors (and destructor)
    public:
        constexpr darray() noexcept(std::is_nothrow_default_constructible_v<Alloc>):
            _cpair(zero_then_variadic{})
        {}

        constexpr darray(const Alloc& new_alloc)
            //Note: N4868 asserts allocators must be nothrow copy constructible.
            noexcept(std::is_nothrow_default_constructible_v<pointer>):
            _cpair(one_then_variadic{}, new_alloc)
        {}

        constexpr darray(const darray& other, const Alloc& alloc):
            darray(alloc)
        {
            _unallocated_assign(
                other._data().first,
                other._data().last,
                other.capacity());
        }

        constexpr darray(const darray& other):
            darray(other, _alloc_traits::select_on_container_copy_construction(other._alloc())) {}

        constexpr darray(darray&& other, const Alloc& alloc)
            noexcept(_alloc_traits::is_always_equal::value &&
                     std::is_nothrow_copy_constructible_v<Alloc>):
            darray(alloc)
        {
            if constexpr (!_alloc_traits::is_always_equal::value) {
                //On allocators compare false: Individually move, others' data
                //cannot be deallocated using new alloc
                if (_alloc() != other._alloc()) {
                    _unallocated_assign(
                        std::make_move_iterator(other._data().first),
                        std::make_move_iterator(other._data().last),
                        other.capacity());

                    return;
                }
            }

            //Steal contents
            _data().steal(std::move(other._data()));
        }

        constexpr darray(darray&& other)
            noexcept:
            _cpair(one_then_variadic{},
                   std::move(other._alloc()),
                   std::exchange(other._data().first, nullptr),
                   std::exchange(other._data().last , nullptr),
                   std::exchange(other._data().end  , nullptr))
        {}

        constexpr ~darray() noexcept
        {
            _clear_dealloc();
        }

    public:
        template<
            std::input_iterator InputIt,
            std::sentinel_for<InputIt> Sentinel>
        requires(!std::forward_iterator<InputIt>)
        constexpr darray(InputIt first, const Sentinel last, const Alloc& alloc = Alloc()):
            darray(alloc)
        {
            for (; first != last; ++first)
                emplace_back(*first);
        }

        template<
            std::forward_iterator FwdIt,
            std::sentinel_for<FwdIt> Sentinel>
        constexpr darray(const FwdIt first, const Sentinel last, const Alloc& alloc = Alloc()):
            darray(alloc)
        {
            _unallocated_assign(first, last, std::ranges::distance(first, last));
        }

    private:
        constexpr void _clear_dealloc()
            noexcept(std::is_nothrow_destructible_v<value_type>)
        {
            if (_data().first) {
                destroy_range(_alloc(), _data().first, _data().last);
                _alloc_traits::deallocate(_alloc(), _data().first, capacity());
            }
        }

        constexpr void _replace(const pointer new_first, const pointer new_last, const size_type new_capacity)
            noexcept(noexcept(_clear_dealloc()))
        {
            _clear_dealloc();

            _data().first = new_first;
            _data().last  = new_last;
            _data().end   = new_first + new_capacity;
        }

    private: //Helper assign functions
        template<
            std::input_iterator InputIt,
            std::sentinel_for<InputIt> Sentinel>
        constexpr void _unallocated_assign(const InputIt first, const Sentinel last, const size_type capacity)
        {
            _data().last = _ctg_duplicate(_alloc(), first, last, _data().first, capacity);
            _data().end  = _data().first + capacity;
        }

        //In order: Allocates new buffer of specified capacity, copies range into new buffer,
        //then overrides internal array. Provides strong guarantee.
        template<
            std::input_iterator InputIt,
            std::sentinel_for<InputIt> Sentinel>
        constexpr void _resize_assign(Alloc& alloc, const InputIt first, const Sentinel last, const size_type new_capacity)
        {
            pointer new_first = nullptr;
            pointer new_last  = _ctg_duplicate(alloc, first, last, new_first, new_capacity);

            _replace(new_first, new_last, new_capacity);
        }

    public:

        //Assign range, utilising a different allocator for allocations and construction.
        //Provides strong guarantee on expansion, weak guarantee otherwise.
        //Note: De-allocation and destruction are performed using internal allocator.
        template<
            std::forward_iterator FwdIt,
            std::sentinel_for<FwdIt> Sentinel>
        constexpr darray& _alt_alloc_assign(Alloc& alt_alloc, FwdIt first, const Sentinel last)
        {
            const size_type range_size = static_cast<size_type>(std::ranges::distance(first, last));

            //Case 1: Not enough capacity to fit new range, resize.
            if (capacity() < range_size) {
                _resize_assign(alt_alloc, first, last, range_size);
            }
            //Case 2: Enough capacity, but new range greater than size. Copy assign, then uninit copy remaining.
            else if (size() < range_size) {
                first = copy_until_sentinel(first, _data().first, _data().last);

                _data().last = uninitialised_copy(alt_alloc, first, last, _data().last);
            }
            //Case 3: New range smaller than size; assign range then destroy remaining.
            else {
                pointer new_last = copy(first, last, _data().first);
                destroy_range(_alloc(), new_last, _data().last);

                _data().last = new_last;
            }

            return *this;
        }

    public:
        template<
            std::input_iterator InputIt,
            std::sentinel_for<InputIt> Sentinel>
        constexpr darray& assign(InputIt first, const Sentinel last)
        {
            if constexpr(std::forward_iterator<InputIt>) {
                return _alt_alloc_assign(_alloc(), first, last);
            }
            else {
                //Case 4: Range size cannot be deduced
                pointer new_last = _data().first;
                for (; new_last != _data().last && first != last; ++first, ++new_last)
                    *new_last = *first;

                destroy_range(_alloc(), new_last, _data().last);
                _data().last = new_last;

                for (; first != last; ++first)
                    emplace_back(*first);

                return *this;
            }
        }

        constexpr darray& operator=(const darray& other)
        {
            if constexpr (_alloc_traits::propagate_on_container_copy_assignment::value) {
                if constexpr (!_alloc_traits::is_always_equal::value) {
                    if (_alloc() != other._alloc()) {
                        _alt_alloc_assign(other._alloc(), other._data().first, other._data().last);
                        _alloc() = other._alloc();
                        return *this;
                    }
                }

                _alloc() = other._alloc();
            }

            return assign(other._data().first, other._data().end);
        }

        constexpr darray& operator=(darray&& other) noexcept
        {
            if constexpr (!_alloc_traits::propagate_on_container_move_assignment::value) {
                if constexpr (!_alloc_traits::is_always_equal::value) {
                    if (_alloc() != other._alloc())
                        return assign(
                            std::make_move_iterator(other._data().first),
                            std::move_sentinel(other._data().last));
                }

                _clear_dealloc();
            }
            else {
                _clear_dealloc();
                _alloc() = std::move(other._alloc());
            }

            _data().steal(std::move(other._data()));
            return *this;
        }

    public:
        constexpr void erase(const const_iterator first, const const_iterator last)
            noexcept(std::is_nothrow_destructible_v<value_type>)
        {
            auto& naked_first = first._unwrapped();

            destroy_range(_alloc(), naked_first, last._unwrapped());
            _data().last = naked_first;
        }

    public:
        //Unchecked emplace_back
        template<class ... Args>
        constexpr void u_emplace_back(Args&& ... args)
            noexcept(std::is_nothrow_constructible_v<value_type, Args...>)
        {
            EXPU_VERIFY_DEBUG(_data().last != _data().end, "Darray has no remaining capacity!");

            _alloc_traits::construct(
                _alloc(),
                std::to_address(_data().last),
                std::forward<Args>(args)...);

            ++_data().last;
        }

        constexpr void upush_back(const value_type& other)
            noexcept(std::is_nothrow_copy_constructible_v<value_type>)
        {
            u_emplace_back(other);
        }

        constexpr void upush_back(value_type&& other)
            noexcept(std::is_nothrow_move_constructible_v<value_type>)
        {
            u_emplace_back(std::move(other));
        }

        template<class ... Args>
        constexpr iterator emplace(const const_iterator at, Args&& ... args)
        {
            const pointer naked_at = at._unwrapped();

            if (_data().last != _data().end) {
                if (naked_at == _data().last)
                    u_emplace_back(std::forward<Args>(args)...);
                else {
                    const pointer before_last = std::prev(_data().last);

                    //If failure to move into uninitialised space, provide strong guarantee
                    if constexpr (std::is_nothrow_move_constructible_v<value_type>)
                        u_emplace_back(std::move(*before_last));
                    else
                        u_emplace_back(*before_last);

                    const pointer post_at = backward_move(naked_at, before_last, std::next(before_last));

                    const auto& at_addr   = std::to_address(naked_at);
                    _alloc_traits::destroy(_alloc(), at_addr);

                    try {
                        _alloc_traits::construct(_alloc(), at_addr, std::forward<Args>(args)...);
                    }
                    //Try to provide strong guarantee
                    catch (...) {
                        //Try to move previously shifted object back one (now uninitialised because it was destroyed).
                        try {
                            _alloc_traits::construct(_alloc(), at_addr, std::move(*post_at));
                        }
                        catch (...) {
                            destroy_range(_alloc(), post_at, _data().last);
                            _data().last = naked_at;
                            throw;
                        }

                        move(std::next(post_at), _data().last, post_at);
                        _alloc_traits::destroy(_alloc(), std::to_address(--_data().last));
                        throw;
                    }

                }

                return iterator(naked_at, &_data());
            }
            //Provide strong guarantee on resize
            else {
                const size_type new_capacity = _calculate_growth(capacity() + 1);

                const pointer new_first    = _alloc_traits::allocate(_alloc(), new_capacity);
                const pointer construct_at = new_first + (naked_at - _data().first);
                      pointer new_last     = new_first;

                //Note: Here the element is emplaced first because of uncertainty on whether it may throw or not.
                //Do not want a scenario where elements are moved to new memory then emplacement throws.
                try {
                    _alloc_traits::construct(_alloc(), std::to_address(construct_at), std::forward<Args>(args)...);
                }
                catch (...) {
                    _alloc_traits::deallocate(_alloc(), new_first, new_capacity);
                    throw;
                }

                try {
                    if (naked_at == _data().last) {
                        new_last = _reversible_uninitialised_move(_data().first, _data().last, new_first);
                        ++new_last;
                    }
                    else {
                        new_last = _reversible_uninitialised_move(_data().first, naked_at, new_first);
                        ++new_last;
                        new_last = _reversible_uninitialised_move(naked_at, _data().last, new_last);
                    }

                }
                catch (...) {
                    destroy_range(_alloc(), new_first, new_last);
                    _alloc_traits::destroy(_alloc(), std::to_address(construct_at));
                    _alloc_traits::deallocate(_alloc(), new_first, new_capacity);
                    throw;
                }

                _replace(new_first, new_last, new_capacity);
                return iterator(construct_at, &_data());
            }

        }

        template<class ... Args>
        constexpr iterator emplace_back(Args&& ... args)
        {
            return emplace(cend(), std::forward<Args>(args)...);
        }

        constexpr void push_back(const value_type& other)
        {
            emplace_back(other);
        }

        constexpr void push_back(value_type&& other)
        {
            emplace_back(std::move(other));
        }

    private:
        //Todo: Consider making non-member
        template<
            std::forward_iterator FwdIt,
            std::sentinel_for<FwdIt> Sentinel>
        constexpr pointer _reversible_uninitialised_move(const FwdIt first, const Sentinel last, const pointer output)
            noexcept(std::is_nothrow_move_constructible_v<std::iter_value_t<FwdIt>>)
        {
            if constexpr (std::is_nothrow_move_constructible_v<std::iter_value_t<FwdIt>>)
                return uninitialised_move(_alloc(), first, last, output);
            else
                return uninitialised_copy(_alloc(), first, last, output);
        }

    public:
        template<
            std::input_iterator InputIt,
            std::sentinel_for<InputIt> Sentinel>
        requires(!std::forward_iterator<InputIt>)
        constexpr darray& insert(const const_iterator at, InputIt first, const Sentinel last)
        {
            const size_type at_index  = static_cast<size_type>(at._unwrapped() - _data().first);
            const size_type prev_size = size();

            for (; first != last; ++first)
                emplace_back(*first);

            //If insertion occurs at end, do not rotate.
            if (at_index != prev_size) {
                //Todo: Create bespoke version. Look into adding simd instructions to speed up
                //for random access iterators
                std::rotate(
                    _data().first + at_index,
                    _data().first + prev_size,
                    _data().last);
            }

            return *this;
        }

        template<
            std::forward_iterator FwdIt,
            std::sentinel_for<FwdIt> Sentinel>
        constexpr void insert(const const_iterator at, FwdIt first, const Sentinel last)
        {
            const auto naked_at = at._unwrapped();

            EXPU_VERIFY_DEBUG((_data().first <= naked_at) && (naked_at <= _data().last),
                "Insertion at pointer does not lie within constructed range (or one after the end) of the array!");

            const auto range_size      = static_cast<size_type>(std::ranges::distance(first, last));
            const auto unused_capacity = static_cast<size_type>(_data().end - _data().last);

            //Avoid invalidating iterators
            if (range_size == 0);
            //Need to reallocate
            else if (unused_capacity < range_size) {
                //Todo: Consider insert function that doesn't grow geometrically
                const auto new_capacity = _calculate_growth(size() + range_size);

                const pointer new_first = _alloc_traits::allocate(_alloc(), new_capacity);
                      pointer new_last  = nullptr;

                pointer constructed_last = new_first + (naked_at - _data().first);
                try {
                    new_last = uninitialised_copy(_alloc(), first, last, constructed_last);
                }
                catch (...) {
                    _alloc_traits::deallocate(_alloc(), new_first, new_capacity);
                    throw;
                }

                try {
                    _reversible_uninitialised_move(_data().first, naked_at, new_first);
                    constructed_last = new_first;
                    new_last = _reversible_uninitialised_move(naked_at, _data().last, new_last);
                }
                catch (...) {
                    //Destroy partially constructed range
                    destroy_range(_alloc(), constructed_last, new_last);
                    _alloc_traits::deallocate(_alloc(), new_first, new_capacity);
                    throw;
                }

                _replace(new_first, new_last, new_capacity);
            }
            //todo: STL does something weird here, they avoid assignment and instead
            //destroy then reconstruct the elements into position. Possible explanation:
            //May be faster for trivially destructible types that are also memcpyable
            // e.g. fundamentals. Test importance of this (Note: not implemented below).
            else {
                const size_type shift_count = _data().last - naked_at;

                pointer insert_end = nullptr;
                pointer new_last   = nullptr;

                // Insert range does not overlap uninitialised space.
                if (range_size < shift_count) {
                    pointer uninit_shift_begin = _data().last - range_size;

                    //Shift as many elements as possible into uninitialised space
                    new_last = _reversible_uninitialised_move(uninit_shift_begin, _data().last, _data().last);

                    //Shift elements by assignment starting from previous last element.
                    if constexpr (std::is_nothrow_move_assignable_v<value_type>)
                        insert_end = backward_move(naked_at, uninit_shift_begin, _data().last);
                    else {
                        try {
                            insert_end = backward_copy(naked_at, uninit_shift_begin, _data().last);
                        }
                        catch (...) {
                            try { move(_data().last, new_last, uninit_shift_begin); }
                            catch(...) {
                                //Provide weak-guarantee
                                _data().last = new_last;
                                throw;
                            }

                            destroy_range(_alloc(), _data().last, new_last);
                            throw;
                        }
                    }

                    try {
                        copy(first, last, naked_at);
                    }
                    catch (...) {
                        try { move(insert_end, new_last, naked_at); }
                        catch (...) {
                            //Provide weak-guarantee
                            _data().last = new_last;
                            throw;
                        }

                        destroy_range(_alloc(), _data().last, new_last);
                        throw;
                    }
                }
                else {
                    pointer uninit_shift_dest = naked_at + range_size;

                    new_last = _reversible_uninitialised_move(naked_at, _data().last, uninit_shift_dest);

                    try {
                        //Assign as much as possible
                        first = copy_until_sentinel(first, naked_at, _data().last);
                        //Uninitialised copy rest
                        uninitialised_copy(_alloc(), first, last, _data().last);
                    }
                    catch (...) {
                        try { move(uninit_shift_dest, new_last, naked_at); }
                        catch (...) { /* Provide weak-guarantee. */ }

                        destroy_range(_alloc(), uninit_shift_dest, new_last);
                        throw;
                    }
                }

                _data().last = new_last;
            }
        }

    private:
        constexpr void _unchecked_grow_exactly(const size_type new_capacity)
        {
            if constexpr (std::is_nothrow_move_constructible_v<value_type>) {
                const pointer new_first = _alloc_traits::allocate(_alloc(), new_capacity);
                //Note: Below will not throw, hence strong guarantee provided by _resize_assign is redundant
                const pointer new_last  = uninitialised_move(_alloc(), _data().first, _data().last, std::to_address(new_first));

                _replace(new_first, new_last, new_capacity);
            }
            else
                _resize_assign(_alloc(), _data().first, _data().end, new_capacity);
        }

        constexpr size_type _calculate_growth(const size_type min_capacity) {
            if (max_size() < min_capacity)
                throw std::bad_array_new_length();

            const size_type half_size = size() >> 1;

            if (max_size() - half_size < size())
                return max_size();
            else
                return std::max(min_capacity, size() + half_size);
        }

        constexpr void _grow_geometric(const size_type min_capacity)
        {
            _unchecked_grow_exactly(_calculate_growth(min_capacity));
        }

    public:
        constexpr void reserve(const size_type size)
        {
            if (capacity() < size)
                _unchecked_grow_exactly(size);
        }

        constexpr void shrink_to_fit()
            noexcept(std::is_nothrow_move_constructible_v<value_type> || std::is_nothrow_copy_constructible_v<value_type>)
        {
            //In the case where no shrinking can be done, avoid invalidating iterators
            if (_data().last != _data().end) {
                const pointer new_first = _alloc_traits::allocate(_alloc(), size());
                      pointer new_last  = nullptr;

                try {
                    new_last = _reversible_uninitialised_move(_data().first, _data().last, new_first);
                }
                catch (...) {
                    _alloc_traits::deallocate(new_first, size());
                    throw;
                }

                _replace(new_first, new_last, size());
            }
        }

    //Indexing functions
    public:
        [[nodiscard]] constexpr const_reference operator[](const size_type index) const noexcept
        {
            EXPU_VERIFY_DEBUG(index < size(), "Index out of range!");
            return *(_data().first + index);
        }

        [[nodiscard]] constexpr reference operator[](const size_type index) noexcept
        {
            return const_cast<reference>(static_cast<const darray&>(*this).operator[](index));
        }

        [[nodiscard]] constexpr const_reference unchecked_front() const noexcept
        {
            EXPU_VERIFY_DEBUG(!empty(), "expu::darray is empty, no viable first value available.");
            return *_data().first;
        }

        [[nodiscard]] constexpr reference unchecked_front() noexcept
        {
            return const_cast<reference>(static_cast<const darray&>(*this).unchecked_front());
        }


        [[nodiscard]] constexpr const_reference unchecked_back() const noexcept
        {
            EXPU_VERIFY_DEBUG(!empty(), "expu::darray is empty, no viable last value available.");
            return *std::prev(_data().last);
        }

        [[nodiscard]] constexpr reference unchecked_back() noexcept
        {
            return const_cast<reference>(static_cast<const darray&>(*this).unchecked_back());
        }

        [[nodiscard]] constexpr const_reference front() const
        {
            if (!empty())
                return unchecked_front();
            else
                throw std::out_of_range("expu::darray is empty, no viable first value available.");
        }

        [[nodiscard]] constexpr reference front()
        {
            return const_cast<reference>(static_cast<const darray&>(*this).front());
        }

        [[nodiscard]] constexpr const_reference back() const
        {
            if (!empty())
                return unchecked_back();
            else
                throw std::out_of_range("expu::darray is empty, no viable last value available.");
        }

        [[nodiscard]] constexpr reference back()
        {
            return const_cast<reference>(static_cast<const darray&>(*this).back());
        }

    //Size getters
    public:
        [[nodiscard]] constexpr size_type size() const noexcept
        {
            return static_cast<size_type>(_data().last - _data().first);
        }

        [[nodiscard]] constexpr size_type capacity() const noexcept
        {
            return static_cast<size_type>(_data().end - _data().first);
        }

        [[nodiscard]] constexpr size_type max_size() noexcept
        {
            return _alloc_traits::max_size(_alloc());
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return _data().last == _data().first;
        }

    //Range getters
    public:
        [[nodiscard]] constexpr iterator begin()              noexcept { return iterator(_data().first, &_data()); }
        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return const_iterator(_data().first, &_data()); }
        [[nodiscard]] constexpr const_iterator begin()  const noexcept { return cbegin(); }

        [[nodiscard]] constexpr iterator end()              noexcept { return iterator(_data().last, &_data()); }
        [[nodiscard]] constexpr const_iterator cend() const noexcept { return const_iterator(_data().last, &_data()); }
        [[nodiscard]] constexpr const_iterator end()  const noexcept { return cend(); }

    public:
        [[nodiscard]] constexpr allocator_type get_allocator() const noexcept { return _alloc(); }

    //Private compressed pair access getters
    private:
        [[nodiscard]] constexpr       _data_t& _data()       noexcept { return _cpair.second(); }
        [[nodiscard]] constexpr const _data_t& _data() const noexcept { return _cpair.second(); }

        [[nodiscard]] constexpr       allocator_type& _alloc()       noexcept { return _cpair.first(); }
        [[nodiscard]] constexpr const allocator_type& _alloc() const noexcept { return _cpair.first(); }

    private:
        compressed_pair<allocator_type, _data_t> _cpair;
    };

}

#endif // !EXPU_CONTAINERS_DARRAY_HPP_INCLUDED